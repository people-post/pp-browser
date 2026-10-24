#include "foundation/data/MeshRole.h"

#include "foundation/platform/Platform.h"
#include "mesh/types/MeshHopDial.h"
#include "common/MeshAlias.h"

#include <algorithm>
#include <unordered_set>

namespace pbr {

MeshRole ResolveMeshRole(const MeshConfig& config) {
  if (Platform::IsMobile()) {
    return MeshRole::Client;
  }
  return config.node_enabled ? MeshRole::Node : MeshRole::Client;
}

void NormalizeMeshConfig(MeshConfig& config) {
  if (!config.bootstrap_peers.empty()) {
    std::unordered_set<std::string> retired;
    retired.reserve(kRetiredMeshBootstrapPeerIdCount);
    for (std::size_t i = 0; i < kRetiredMeshBootstrapPeerIdCount; ++i) {
      retired.insert(kRetiredMeshBootstrapPeerIds[i]);
    }
    config.bootstrap_peers.erase(
        std::remove_if(config.bootstrap_peers.begin(), config.bootstrap_peers.end(),
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

std::vector<std::string> DialWorthyMultiaddrs(std::vector<std::string> multiaddrs) {
  const auto local_scope = [](const std::string& ma) {
    const bool ip = ma.rfind("/ip6/", 0) == 0 || ma.find("/ip4/") != std::string::npos;
    return ip && !mesh::MultiaddrHasPublicDialHost(ma);
  };
  if (!std::all_of(multiaddrs.begin(), multiaddrs.end(), local_scope)) {
    std::erase_if(multiaddrs, local_scope);
  }
  return multiaddrs;
}

std::vector<std::string> ResolveEffectiveBootstrapPeers(const MeshConfig& config,
                                                        const std::vector<mesh::MeshDirectoryNode>& directory_nodes) {
  MeshConfig normalized = config;
  NormalizeMeshConfig(normalized);

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

  // L0: config file bootstrap_peers, else hardcoded defaults (via Normalize).
  for (const std::string& ma : normalized.bootstrap_peers) {
    append_ma(ma);
  }
  return out;
}

} // namespace pbr
