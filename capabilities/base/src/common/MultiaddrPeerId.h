#pragma once

#include <string>

namespace pbr {

/** Extract PeerId after `/p2p/` (last component). */
inline std::string PeerIdFromMultiaddr(const std::string& multiaddr) {
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
