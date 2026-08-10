#pragma once

#include "base/data/Libp2pRole.h"
#include "libp2p/integration/host/Libp2pHost.h"

#include <string>

namespace pbr {
namespace test {

/**
 * Bind a loopback libp2p host on an OS-assigned port (`/tcp/0`).
 *
 * Fixed pid-derived ports collide across ctest processes on macOS/Windows when
 * prior listeners leave sockets in TIME_WAIT; ephemeral listen avoids that.
 */
inline Roe<void> StartEphemeralLoopbackHost(Libp2pHost& host, int& out_port) {
  Libp2pHostConfig cfg;
  cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/0";
  auto started = host.Start(cfg);
  if (!started) {
    return started.error();
  }
  for (const auto& ma : host.ListenMultiaddrs()) {
    if (auto port = TcpPortFromMultiaddr(ma); port && *port > 0) {
      out_port = *port;
      return {};
    }
  }
  host.Stop();
  return Error("libp2p ephemeral listen produced no tcp port");
}

inline std::string LoopbackP2pMultiaddr(int port, const std::string& peer_id) {
  return "/ip4/127.0.0.1/tcp/" + std::to_string(port) + "/p2p/" + peer_id;
}

} // namespace test
} // namespace pbr
