#include "foundation/data/MeshRole.h"

#include "foundation/platform/Platform.h"

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
    return;
  }
  config.bootstrap_peers.reserve(kDefaultMeshBootstrapPeerCount);
  for (std::size_t i = 0; i < kDefaultMeshBootstrapPeerCount; ++i) {
    config.bootstrap_peers.emplace_back(kDefaultMeshBootstrapPeers[i]);
  }
}

std::vector<std::string> ResolveEffectiveBootstrapPeers(const MeshConfig& config,
                                                        const std::vector<MeshDirectoryNode>& directory_nodes) {
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
  for (const MeshDirectoryNode& node : directory_nodes) {
    if (node.peer_id.empty()) {
      continue;
    }
    if (!seen.insert(node.peer_id).second) {
      continue;
    }
    std::string ma;
    for (const std::string& candidate : node.multiaddrs) {
      if (!candidate.empty() && PeerIdFromMultiaddr(candidate) == node.peer_id) {
        ma = candidate;
        break;
      }
    }
    if (ma.empty() && !node.multiaddrs.empty()) {
      ma = node.multiaddrs.front();
    }
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

std::string PeerIdFromMultiaddr(const std::string& multiaddr) {
  const std::string marker = "/p2p/";
  const auto pos = multiaddr.rfind(marker);
  if (pos == std::string::npos) {
    return {};
  }
  std::string id = multiaddr.substr(pos + marker.size());
  const auto slash = id.find('/');
  if (slash != std::string::npos) {
    id.resize(slash);
  }
  return id;
}

} // namespace pbr
