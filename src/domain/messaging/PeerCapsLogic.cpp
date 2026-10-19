#include "domain/messaging/PeerCapsLogic.h"

#include "foundation/data/MeshRole.h"
#include "common/MeshAlias.h"

#include <unordered_set>

namespace pbr {

std::vector<mesh::MeshHopCandidate> FilterHopsByMediaRelayAds(
    std::vector<mesh::MeshHopCandidate> ranked,
    const std::function<bool(const std::string& peer_id)>& has_media_relay_ad) {
  std::vector<mesh::MeshHopCandidate> out;
  out.reserve(ranked.size());
  for (mesh::MeshHopCandidate& hop : ranked) {
    if (hop.affinity == mesh::MeshHopAffinity::OrgSeed) {
      out.push_back(std::move(hop));
      continue;
    }
    if (hop.affinity == mesh::MeshHopAffinity::DirectoryNode) {
      if (hop.advertises_media_relay) {
        out.push_back(std::move(hop));
      }
      continue;
    }
    if (hop.affinity == mesh::MeshHopAffinity::DhtDiscovered) {
      if (hop.advertises_media_relay) {
        out.push_back(std::move(hop));
      }
      continue;
    }
    if (has_media_relay_ad && has_media_relay_ad(hop.peer_id)) {
      out.push_back(std::move(hop));
    }
  }
  return out;
}

std::vector<mesh::MeshHopCandidate> MergeAdvertisedMediaRelayHops(
    std::vector<mesh::MeshHopCandidate> ranked, const std::vector<std::string>& advertised_media_relay_peer_ids,
    const std::function<std::string(const std::string& peer_id)>& resolve_multiaddr) {
  if (advertised_media_relay_peer_ids.empty()) {
    return ranked;
  }
  std::unordered_set<std::string> seen;
  for (const mesh::MeshHopCandidate& hop : ranked) {
    if (!hop.peer_id.empty()) {
      seen.insert(hop.peer_id);
    }
  }
  std::vector<mesh::MeshHopCandidate> injected;
  injected.reserve(advertised_media_relay_peer_ids.size());
  for (const std::string& peer_id : advertised_media_relay_peer_ids) {
    if (peer_id.empty() || !seen.insert(peer_id).second) {
      continue;
    }
    mesh::MeshHopCandidate hop;
    hop.peer_id = peer_id;
    hop.affinity = mesh::MeshHopAffinity::Contact;
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

bool DirectoryNodeAdvertisesMediaRelay(const mesh::MeshDirectoryNode& node) {
  return node.media_relay && !node.peer_id.empty();
}

bool PeerHasMediaRelayInDirectory(const std::string& peer_id,
                                  const std::vector<mesh::MeshDirectoryNode>& nodes) {
  if (peer_id.empty()) {
    return false;
  }
  for (const mesh::MeshDirectoryNode& node : nodes) {
    if (node.peer_id == peer_id && DirectoryNodeAdvertisesMediaRelay(node)) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> MediaRelayPeerIdsFromDirectory(const std::vector<mesh::MeshDirectoryNode>& nodes) {
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  for (const mesh::MeshDirectoryNode& node : nodes) {
    if (!DirectoryNodeAdvertisesMediaRelay(node) || !seen.insert(node.peer_id).second) {
      continue;
    }
    out.push_back(node.peer_id);
  }
  return out;
}

std::vector<std::string> MergeMediaRelayCapablePeerIds(
    const std::vector<std::string>& from_call_caps, const std::vector<mesh::MeshDirectoryNode>& directory_nodes,
    const std::vector<mesh::MeshDirectoryNode>& dht_nodes) {
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  auto append = [&](const std::string& peer_id) {
    if (peer_id.empty() || !seen.insert(peer_id).second) {
      return;
    }
    out.push_back(peer_id);
  };
  for (const std::string& peer_id : from_call_caps) {
    append(peer_id);
  }
  for (const std::string& peer_id : MediaRelayPeerIdsFromDirectory(directory_nodes)) {
    append(peer_id);
  }
  for (const std::string& peer_id : MediaRelayPeerIdsFromDirectory(dht_nodes)) {
    append(peer_id);
  }
  return out;
}

} // namespace pbr
