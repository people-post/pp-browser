#include "base/data/Libp2pRole.h"

#include "base/platform/Platform.h"

#include <cctype>
#include <cstdlib>

namespace pbr {

namespace {

std::string IpPrefixBeforeTcp(const std::string& multiaddr) {
  const auto tcp_pos = multiaddr.find("/tcp/");
  if (tcp_pos == std::string::npos) {
    return multiaddr;
  }
  return multiaddr.substr(0, tcp_pos);
}

} // namespace

Libp2pRole ResolveLibp2pRole(const Libp2pConfig& config) {
  if (Platform::IsMobile()) {
    return Libp2pRole::Client;
  }
  return config.node_enabled ? Libp2pRole::Node : Libp2pRole::Client;
}

void NormalizeLibp2pConfig(Libp2pConfig& config) {
  if (config.listen_multiaddr.empty()) {
    config.listen_multiaddr = kPreferredLibp2pListenMultiaddr;
  }
  if (config.bootstrap_peers.empty()) {
    config.bootstrap_peers.push_back(kDefaultLibp2pBootstrapPeer);
  }
}

std::optional<int> TcpPortFromMultiaddr(const std::string& multiaddr) {
  const std::string marker = "/tcp/";
  const auto pos = multiaddr.find(marker);
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  size_t i = pos + marker.size();
  if (i >= multiaddr.size() || !std::isdigit(static_cast<unsigned char>(multiaddr[i]))) {
    return std::nullopt;
  }
  char* end = nullptr;
  const long port = std::strtol(multiaddr.c_str() + static_cast<std::ptrdiff_t>(i), &end, 10);
  if (end == multiaddr.c_str() + static_cast<std::ptrdiff_t>(i) || port < 0 || port > 65535) {
    return std::nullopt;
  }
  return static_cast<int>(port);
}

std::string ReplaceTcpPortInMultiaddr(const std::string& multiaddr, int port) {
  if (port < 0 || port > 65535) {
    return {};
  }
  const std::string marker = "/tcp/";
  const auto pos = multiaddr.find(marker);
  if (pos == std::string::npos) {
    return {};
  }
  size_t i = pos + marker.size();
  while (i < multiaddr.size() && std::isdigit(static_cast<unsigned char>(multiaddr[i]))) {
    ++i;
  }
  return multiaddr.substr(0, pos + marker.size()) + std::to_string(port) + multiaddr.substr(i);
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

std::vector<std::string> BuildLibp2pListenCandidates(const std::string& preferred_multiaddr) {
  std::vector<std::string> out;
  const std::string preferred =
      preferred_multiaddr.empty() ? std::string(kPreferredLibp2pListenMultiaddr) : preferred_multiaddr;
  out.push_back(preferred);

  const auto port = TcpPortFromMultiaddr(preferred);
  if (port && *port >= kPreferredLibp2pListenPort && *port <= kLibp2pListenFallbackPortEnd) {
    for (int p = kPreferredLibp2pListenPort; p <= kLibp2pListenFallbackPortEnd; ++p) {
      if (p == *port) {
        continue;
      }
      std::string next = ReplaceTcpPortInMultiaddr(preferred, p);
      if (!next.empty()) {
        out.push_back(std::move(next));
      }
    }
  } else if (port && *port > 0) {
    for (int delta = 1; delta <= 9; ++delta) {
      const int p = *port + delta;
      if (p > 65535) {
        break;
      }
      std::string next = ReplaceTcpPortInMultiaddr(preferred, p);
      if (!next.empty()) {
        out.push_back(std::move(next));
      }
    }
  }

  const std::string prefix = IpPrefixBeforeTcp(preferred);
  if (!prefix.empty()) {
    out.push_back(prefix + "/tcp/0");
  }
  return out;
}

} // namespace pbr
