#include "base/data/Libp2pRole.h"

#include "base/platform/Platform.h"

namespace pbr {

Libp2pRole ResolveLibp2pRole(const Libp2pConfig& config) {
  if (Platform::IsMobile()) {
    return Libp2pRole::Client;
  }
  return config.node_enabled ? Libp2pRole::Node : Libp2pRole::Client;
}

void NormalizeLibp2pConfig(Libp2pConfig& config) {
  if (config.bootstrap_peers.empty()) {
    config.bootstrap_peers.push_back(kDefaultLibp2pBootstrapPeer);
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
