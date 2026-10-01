#include "feature/calls/CallHopRanking.h"

#include "domain/messaging/PeerCapsLogic.h"

#include <unordered_map>
#include "common/PbrCompat.h"

namespace pbr {

void CallHopRanking::FillDialInfo(std::vector<MeshHopCandidate>& ranked, const bool published_ma_is_dialable) const {
  if (!deps_->dial) {
    return;
  }
  for (MeshHopCandidate& hop : ranked) {
    if (hop.multiaddr.empty()) {
      if (auto ma = deps_->dial->PreferredMultiaddr(hop.peer_id)) {
        hop.multiaddr = *ma;
      }
    }
    hop.dialable = deps_->dial->IsDialable(hop.peer_id);
    if (published_ma_is_dialable) {
      hop.dialable = hop.dialable || !hop.multiaddr.empty();
    } else if (!hop.dialable && !hop.multiaddr.empty() &&
               (hop.affinity == MeshHopAffinity::OrgSeed || hop.affinity == MeshHopAffinity::DirectoryNode ||
                hop.affinity == MeshHopAffinity::DhtDiscovered)) {
      // Directory / bootstrap seeds with a published MA are SoftMigrate candidates even before
      // Amp address-book learn (cross-net PreferLocal fallback).
      hop.dialable = true;
    }
  }
}

std::vector<MeshHopCandidate> CallHopRanking::Ranked() const {
  if (!deps_) {
    return {};
  }
  std::vector<Contact> contacts;
  if (auto listed = contacts_.List()) {
    contacts = std::move(*listed);
  }
  const std::vector<MeshDirectoryNode> directory_nodes =
      deps_->list_directory_nodes ? deps_->list_directory_nodes() : std::vector<MeshDirectoryNode>{};
  const std::vector<MeshDirectoryNode> dht_nodes =
      deps_->list_dht_nodes ? deps_->list_dht_nodes() : std::vector<MeshDirectoryNode>{};
  const bool include_seeds = !deps_->seed_dial_ok || deps_->seed_dial_ok();
  auto merged = BuildCircuitHopList(contacts, directory_nodes, dht_nodes, deps_->bootstrap_peers,
                                    deps_->prefer_contacts, include_seeds);
  auto ranked = RankMediaHopsEscalating(std::move(merged), deps_->prefer_contacts, deps_->local_listen_multiaddr);
  if (const std::string self = LocalPeerId(); !self.empty()) {
    ranked = ExcludeSelfHop(std::move(ranked), self);
  }
  FillDialInfo(ranked, false);
  // V030: contacts need media_relay ads; org seeds always eligible; PreferLocal added later.
  ranked = FilterHopsByMediaRelayAds(std::move(ranked), deps_->peer_has_media_relay);
  if (deps_->list_media_relay_peers) {
    ranked = MergeAdvertisedMediaRelayHops(std::move(ranked), deps_->list_media_relay_peers(),
                                           [this](const std::string& peer_id) -> std::string {
                                             if (!deps_->dial) {
                                               return {};
                                             }
                                             auto ma = deps_->dial->PreferredMultiaddr(peer_id);
                                             return ma ? *ma : std::string{};
                                           });
    FillDialInfo(ranked, true);
  }
  if (deps_->trusted_relays_only) {  // privacy T4: a relay sees both ends' addresses
    ranked = KeepTrustedRelays(std::move(ranked), TrustedRelayPeerIds(contacts, deps_->bootstrap_peers));
  }
  return ranked;
}

bool CallHopRanking::HasCandidates() const {
  if (!deps_ || !deps_->relay || !deps_->dial) {
    return false;
  }
  if (deps_->prefer_local_as_hop && deps_->relay->IsStarted()) {
    return true;
  }
  for (const MeshHopCandidate& hop : Ranked()) {
    if (hop.dialable) {
      return true;
    }
  }
  return false;
}

std::string CallHopRanking::HopMultiaddr(const std::string& hop_peer_id) const {
  if (hop_peer_id.empty()) {
    return {};
  }
  for (const MeshHopCandidate& hop : Ranked()) {
    if (hop.peer_id == hop_peer_id && !hop.multiaddr.empty()) {
      return hop.multiaddr;
    }
  }
  if (deps_ && deps_->dial) {
    if (auto ma = deps_->dial->PreferredMultiaddr(hop_peer_id)) {
      return *ma;
    }
  }
  return {};
}

std::vector<std::string> CallHopRanking::LocalAdvertiseMas() const {
  if (!deps_) {
    return {};
  }
  if (deps_->resolve_local_advertise) {
    auto live = deps_->resolve_local_advertise();
    if (!live.empty()) {
      return live;
    }
  }
  if (!deps_->local_advertise_multiaddrs.empty()) {
    return deps_->local_advertise_multiaddrs;
  }
  if (!deps_->local_listen_multiaddr.empty()) {
    return {deps_->local_listen_multiaddr};
  }
  return {};
}

std::string CallHopRanking::LocalAdvertiseMa(const std::string& local_peer_id) const {
  const auto mas = LocalAdvertiseMas();
  if (mas.empty()) {
    return {};
  }
  std::string local_ma = mas.front();
  if (!local_peer_id.empty() && local_ma.find("/p2p/") == std::string::npos) {
    local_ma += "/p2p/" + local_peer_id;
  }
  return local_ma;
}

CallHopScope CallHopRanking::ScopeForPeers(const std::vector<std::string>& remote_identities) const {
  std::unordered_map<std::string, std::vector<std::string>> known;
  if (deps_ && deps_->resolve_remote_listen_by_peer) {
    known = deps_->resolve_remote_listen_by_peer();
  }
  // Missing entry → empty vector → Wide.
  std::unordered_map<std::string, std::vector<std::string>> remotes;
  for (const std::string& identity : remote_identities) {
    auto it = known.find(identity);
    remotes[identity] = it != known.end() ? it->second : std::vector<std::string>{};
  }
  return InferCallHopScope(LocalAdvertiseMas(), remotes);
}

bool CallHopRanking::LanConfirmedForPeers(const std::vector<std::string>& remote_identities) const {
  if (!deps_ || !deps_->peer_lan_confirmed) {
    return false;
  }
  std::unordered_map<std::string, std::vector<std::string>> remotes;
  if (deps_->resolve_remote_listen_by_peer) {
    remotes = deps_->resolve_remote_listen_by_peer();
  }
  for (const std::string& identity : remote_identities) {
    if (deps_->peer_lan_confirmed(identity)) {
      return true;
    }
    auto it = remotes.find(identity);
    if (it == remotes.end()) {
      continue;
    }
    for (const std::string& ma : it->second) {
      const auto p2p = ma.rfind("/p2p/");
      if (p2p == std::string::npos) {
        continue;
      }
      std::string peer_id = ma.substr(p2p + 5);
      if (const auto slash = peer_id.find('/'); slash != std::string::npos) {
        peer_id.resize(slash);
      }
      if (!peer_id.empty() && deps_->peer_lan_confirmed(peer_id)) {
        return true;
      }
    }
  }
  return false;
}

std::string CallHopRanking::LocalPeerId() const {
  if (deps_ && deps_->relay) {
    if (auto pid = deps_->relay->LocalPeerIdBase58()) {
      return *pid;
    }
  }
  return {};
}

} // namespace pbr
