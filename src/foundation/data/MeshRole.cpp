#include "foundation/data/MeshRole.h"

#include "foundation/platform/Platform.h"
#include "mesh/types/MeshHopDial.h"
#include "common/MeshAlias.h"

#include <algorithm>
#include <unordered_set>

namespace pbr {

MeshRole ResolveMeshRole(const MeshConfig& config) {
  return MeshRoleOf(EffectiveMeshConfig(config, Platform::IsMobile()));
}

void NormalizeMeshConfig(MeshConfig& config) {
  if (!config.bootstrap_peers.empty()) {
    std::unordered_set<std::string> retired;
    retired.reserve(kRetiredMeshBootstrapPeerIdCount);
    for (std::size_t i = 0; i < kRetiredMeshBootstrapPeerIdCount; ++i) {
      retired.insert(kRetiredMeshBootstrapPeerIds[i]);
    }
    config.bootstrap_peers.erase(std::remove_if(config.bootstrap_peers.begin(), config.bootstrap_peers.end(),
                                                [&retired](const std::string& ma) {
                                                  const std::string peer_id = PeerIdFromMultiaddr(ma);
                                                  return !peer_id.empty() && retired.count(peer_id) > 0;
                                                }),
                                 config.bootstrap_peers.end());
  }
  if (!config.bootstrap_peers.empty()) {
    return;
  }
  config.bootstrap_peers.reserve(kDefaultMeshBootstrapPeerCount);
  for (std::size_t i = 0; i < kDefaultMeshBootstrapPeerCount; ++i) {
    config.bootstrap_peers.emplace_back(kDefaultMeshBootstrapPeers[i]);
  }
}

std::vector<std::string> ResolveEffectiveBootstrapPeers(const MeshConfig& config,
                                                        const std::vector<mesh::MeshDirectoryNode>& directory_nodes) {
  MeshConfig normalized = config;
  NormalizeMeshConfig(normalized);
  return EffectiveBootstrapPeers(normalized, directory_nodes);
}

} // namespace pbr
