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
  // ListenMultiaddrs prefers interface (actual bound) addresses over listen-request
  // keys so /tcp/0 resolves to the OS-assigned port.
  const auto listened = host.ListenMultiaddrs();
  for (const std::string& ma : listened) {
    if (const auto port = TcpPortFromMultiaddrLocal(ma); port && *port > 0) {
      return ma;
    }
  }
  if (!listened.empty()) {
    return listened.front();
  }
  return requested;
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
  identify_ = std::make_unique<IdentifyIntegrationService>();
  (void)identify_->Start(*host_, sessions_.get());
  RegisterBootstrapPeers(config.bootstrap_peers);
  return {};
}

void NodeRuntime::Stop() {
  StopEphemeralListen();
  if (identify_) {
    identify_->Stop();
    identify_.reset();
  }
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

PeerSessionManager* NodeRuntime::Sessions() const {
  return sessions_.get();
}

IdentifyIntegrationService* NodeRuntime::Identify() {
  return identify_.get();
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

Roe<void> NodeRuntime::StartEphemeralListen() {
  if (ephemeral_listen_active_) {
    return {};
  }
  if (!host_ || !host_->IsRunning()) {
    return Error("libp2p host not running");
  }

  std::vector<std::string> candidates;
  candidates.push_back("/ip4/0.0.0.0/tcp/0");

  Error last_error("mobile ephemeral listen failed");
  for (const std::string& candidate : candidates) {
    auto listened = host_->ListenOn(candidate);
    if (listened) {
      ephemeral_listen_active_ = true;
      bound_listen_ = ResolveBoundListenMultiaddr(*host_, candidate);
      last_error_.clear();
      return {};
    }
    last_error = listened.error();
  }
  last_error_ = last_error.message;
  return last_error;
}

void NodeRuntime::StartEphemeralListenAsync(std::function<void(Roe<void>)> cb) {
  if (!cb) {
    return;
  }
  if (ephemeral_listen_active_) {
    cb({});
    return;
  }
  if (!host_ || !host_->IsRunning()) {
    cb(Error("libp2p host not running"));
    return;
  }
  const std::string candidate = "/ip4/0.0.0.0/tcp/0";
  // cb runs on the libp2p io thread — callers that touch BrowserThread state must hop.
  host_->ListenOnAsync(candidate, [this, candidate, cb = std::move(cb)](Roe<void> listened) mutable {
    if (listened) {
      ephemeral_listen_active_ = true;
      bound_listen_ = ResolveBoundListenMultiaddr(*host_, candidate);
      last_error_.clear();
      cb({});
      return;
    }
    last_error_ = listened.error().message;
    cb(listened.error());
  });
}

void NodeRuntime::StopEphemeralListen() {
  if (!ephemeral_listen_active_) {
    return;
  }
  if (host_ && host_->IsRunning()) {
    (void)host_->StopListening();
  }
  ephemeral_listen_active_ = false;
  bound_listen_.clear();
}

void NodeRuntime::StopEphemeralListenAsync(std::function<void()> cb) {
  if (!ephemeral_listen_active_) {
    if (cb) {
      cb();
    }
    return;
  }
  if (!host_ || !host_->IsRunning()) {
    ephemeral_listen_active_ = false;
    bound_listen_.clear();
    if (cb) {
      cb();
    }
    return;
  }
  // cb runs on the libp2p io thread after listeners are closed.
  host_->StopListeningAsync([this, cb = std::move(cb)]() mutable {
    ephemeral_listen_active_ = false;
    bound_listen_.clear();
    if (cb) {
      cb();
    }
  });
}

} // namespace pbr
