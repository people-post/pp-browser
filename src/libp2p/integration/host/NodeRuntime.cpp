#include "libp2p/integration/host/NodeRuntime.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <optional>

namespace pbr {

namespace {

std::string PeerIdFromMultiaddrLocal(const std::string& multiaddr) {
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

std::optional<int> TcpPortFromMultiaddrLocal(const std::string& multiaddr) {
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

} // namespace

std::string ResolveBoundListenMultiaddr(Libp2pHost& host, const std::string& requested) {
  const auto listened = host.ListenMultiaddrs();
  if (listened.empty()) {
    return requested;
  }
  for (const std::string& ma : listened) {
    if (const auto port = TcpPortFromMultiaddrLocal(ma); port && *port > 0) {
      return ma;
    }
  }
  return listened.front();
}

PeerSessionConfig MakePeerSessionConfig(size_t max_connections, size_t max_concurrent_dials,
                                        int dial_timeout_ms, int idle_ttl_ms,
                                        int dial_failure_backoff_ms) {
  PeerSessionConfig session;
  session.max_connections = max_connections;
  session.max_concurrent_dials = max_concurrent_dials;
  session.dial_timeout = std::chrono::milliseconds(dial_timeout_ms);
  session.idle_ttl = std::chrono::milliseconds(idle_ttl_ms);
  session.dial_failure_backoff = std::chrono::milliseconds(dial_failure_backoff_ms);
  return session;
}

NodeRuntime::NodeRuntime() = default;

NodeRuntime::~NodeRuntime() {
  Stop();
}

void NodeRuntime::RegisterBootstrapPeers(const std::vector<std::string>& bootstrap_peers) {
  if (!sessions_) {
    return;
  }
  size_t index = 0;
  for (const std::string& ma : bootstrap_peers) {
    if (ma.empty()) {
      continue;
    }
    std::string key = PeerIdFromMultiaddrLocal(ma);
    if (key.empty()) {
      key = "bootstrap:" + std::to_string(index);
    }
    (void)sessions_->RegisterEndpoint(key, ma);
    ++index;
  }
}

Roe<void> NodeRuntime::Start(const NodeRuntimeConfig& config) {
  Stop();
  last_error_.clear();
  bound_listen_.clear();

  Error last_error("libp2p host start failed");

  if (!config.host.listen_enabled) {
    host_ = std::make_unique<Libp2pHost>();
    Libp2pHostConfig host_config = config.host;
    auto started = host_->Start(host_config);
    if (!started) {
      last_error_ = started.error().message;
      host_.reset();
      return started.error();
    }
  } else {
    std::vector<std::string> candidates = config.listen_candidates;
    if (candidates.empty()) {
      candidates.push_back(config.host.listen_multiaddr);
    }
    bool started_ok = false;
    for (const std::string& candidate : candidates) {
      host_ = std::make_unique<Libp2pHost>();
      Libp2pHostConfig host_config = config.host;
      host_config.listen_multiaddr = candidate;
      auto started = host_->Start(host_config);
      if (started) {
        started_ok = true;
        bound_listen_ = ResolveBoundListenMultiaddr(*host_, candidate);
        break;
      }
      last_error = started.error();
      host_.reset();
    }
    if (!started_ok) {
      last_error_ = last_error.message;
      return last_error;
    }
  }

  sessions_ = std::make_unique<PeerSessionManager>(*host_, config.sessions);
  RegisterBootstrapPeers(config.bootstrap_peers);
  return {};
}

void NodeRuntime::Stop() {
  sessions_.reset();
  if (host_) {
    host_->Stop();
    host_.reset();
  }
  bound_listen_.clear();
}

bool NodeRuntime::IsRunning() const {
  return host_ && host_->IsRunning();
}

Libp2pHost* NodeRuntime::Host() {
  return host_.get();
}

PeerSessionManager* NodeRuntime::Sessions() {
  return sessions_.get();
}

void NodeRuntime::Tick() {
  if (sessions_) {
    sessions_->Tick();
  }
}

void NodeRuntime::SuspendColdPeers() {
  if (sessions_) {
    sessions_->SuspendColdPeers();
  }
}

} // namespace pbr
