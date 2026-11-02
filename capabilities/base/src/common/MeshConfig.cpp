#include "common/MeshConfig.h"

#include "common/MultiaddrPeerId.h"
#include "mesh/types/MeshHopDial.h"

#include <unordered_set>
#include <utility>

namespace pbr {

std::vector<std::string> EffectiveBootstrapPeers(const MeshConfig& config,
                                                 const std::vector<mesh::MeshDirectoryNode>& directory_nodes) {
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  auto append_ma = [&](const std::string& ma) {
    if (ma.empty()) {
      return;
    }
    const std::string peer_id = PeerIdFromMultiaddr(ma);
    if (peer_id.empty() || !seen.insert(peer_id).second) {
      return;
    }
    out.push_back(ma);
  };

  // L1: Brief/Amp directory mesh_node listings (live snapshot or disk cache).
  for (const mesh::MeshDirectoryNode& node : directory_nodes) {
    if (node.peer_id.empty()) {
      continue;
    }
    if (!seen.insert(node.peer_id).second) {
      continue;
    }
    std::vector<std::string> own;
    for (const std::string& candidate : node.multiaddrs) {
      if (!candidate.empty() && PeerIdFromMultiaddr(candidate) == node.peer_id) {
        own.push_back(candidate);
      }
    }
    std::string ma = mesh::PreferredDialMultiaddr(own.empty() ? node.multiaddrs : own);
    if (ma.empty()) {
      seen.erase(node.peer_id);
      continue;
    }
    out.push_back(std::move(ma));
  }

  // L0: the config's seeds (the app normalized it: its file's, else the deployment's defaults).
  for (const std::string& ma : config.bootstrap_peers) {
    append_ma(ma);
  }
  return out;
}

} // namespace pbr
