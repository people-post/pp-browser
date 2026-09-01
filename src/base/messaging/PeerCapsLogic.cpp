#include "base/messaging/PeerCapsLogic.h"

#include "base/data/MeshRole.h"

#include <unordered_set>

namespace pbr {

std::vector<MeshHopCandidate> FilterHopsByMediaRelayAds(
    std::vector<MeshHopCandidate> ranked,
    const std::function<bool(const std::string& peer_id)>& has_media_relay_ad) {
  std::vector<MeshHopCandidate> out;
  out.reserve(ranked.size());
  for (MeshHopCandidate& hop : ranked) {
    if (hop.affinity == MeshHopAffinity::OrgSeed) {
      out.push_back(std::move(hop));
      continue;
    }
    if (has_media_relay_ad && has_media_relay_ad(hop.peer_id)) {
      out.push_back(std::move(hop));
    }
  }
  return out;
}

std::vector<MeshHopCandidate> MergeAdvertisedMediaRelayHops(
    std::vector<MeshHopCandidate> ranked, const std::vector<std::string>& advertised_media_relay_peer_ids,
    const std::function<std::string(const std::string& peer_id)>& resolve_multiaddr) {
  if (advertised_media_relay_peer_ids.empty()) {
    return ranked;
  }
  std::unordered_set<std::string> seen;
  for (const MeshHopCandidate& hop : ranked) {
    if (!hop.peer_id.empty()) {
      seen.insert(hop.peer_id);
    }
  }
  std::vector<MeshHopCandidate> injected;
  injected.reserve(advertised_media_relay_peer_ids.size());
  for (const std::string& peer_id : advertised_media_relay_peer_ids) {
    if (peer_id.empty() || !seen.insert(peer_id).second) {
      continue;
    }
    MeshHopCandidate hop;
    hop.peer_id = peer_id;
    hop.affinity = MeshHopAffinity::Contact;
    hop.dialable = true;
    if (resolve_multiaddr) {
      hop.multiaddr = resolve_multiaddr(peer_id);
    }
    injected.push_back(std::move(hop));
  }
  if (injected.empty()) {
    return ranked;
  }
  // Prefer advertised call peers ahead of seeds/contacts so phone initiator picks the desktop
  // Node already on the call before an off-LAN seed.
  injected.insert(injected.end(), std::make_move_iterator(ranked.begin()),
                  std::make_move_iterator(ranked.end()));
  return injected;
}

std::vector<std::string> PeerIdsFromListenMultiaddrs(const std::vector<std::string>& multiaddrs) {
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  for (const std::string& ma : multiaddrs) {
    const std::string peer_id = PeerIdFromMultiaddr(ma);
    if (peer_id.empty() || !seen.insert(peer_id).second) {
      continue;
    }
    out.push_back(peer_id);
  }
  return out;
}

} // namespace pbr
