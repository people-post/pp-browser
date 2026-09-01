#include "base/data/MeshRole.h"

#include "base/platform/Platform.h"

namespace pbr {

MeshRole ResolveMeshRole(const MeshConfig& config) {
  if (Platform::IsMobile()) {
    return MeshRole::Client;
  }
  return config.node_enabled ? MeshRole::Node : MeshRole::Client;
}

void NormalizeMeshConfig(MeshConfig& config) {
  if (config.bootstrap_peers.empty()) {
    config.bootstrap_peers.push_back(kDefaultMeshBootstrapPeer);
  }
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
