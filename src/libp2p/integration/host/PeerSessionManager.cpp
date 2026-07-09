#include "libp2p/integration/host/PeerSessionManager.h"

#include <libp2p/host/host.hpp>
#include <libp2p/multi/multiaddress.hpp>
#include <libp2p/network/connection_manager.hpp>
#include <libp2p/network/network.hpp>
#include <libp2p/peer/peer_id.hpp>

#include <algorithm>

#include <system_error>

namespace pbr {

PeerSessionManager::PeerSessionManager(Libp2pHost& host, PeerSessionConfig config)
    : host_(host), config_(std::move(config)), last_sweep_(std::chrono::steady_clock::now()) {}

PeerSessionManager::~PeerSessionManager() = default;

void PeerSessionManager::SetConfig(PeerSessionConfig config) {
  std::lock_guard lock(mutex_);
  config_ = std::move(config);
}

Roe<void> PeerSessionManager::RegisterEndpoint(const std::string& peer_relay_user_id,
                                               const std::string& multiaddr) {
  if (peer_relay_user_id.empty() || multiaddr.empty()) {
    return Error("Empty peer endpoint");
  }
  auto parsed = libp2p::multi::Multiaddress::create(multiaddr);
  if (!parsed) {
    return Error("Invalid multiaddr");
  }
  const libp2p::multi::Multiaddress& address = parsed.value();
  const auto peer_id_str = address.getPeerId();
  if (!peer_id_str) {
    return Error("Multiaddr missing /p2p/ PeerId");
  }
  auto peer_id = libp2p::peer::PeerId::fromBase58(*peer_id_str);
  if (!peer_id) {
    return Error("Invalid PeerId in multiaddr");
  }

  libp2p::peer::PeerInfo info{peer_id.value(), {address}};
  if (host_.IsRunning()) {
    (void)host_.GetHost().getPeerRepository().getAddressRepository().upsertAddresses(
        peer_id.value(), std::span<const libp2p::multi::Multiaddress>(info.addresses),
        std::chrono::hours(24));
  }

  std::lock_guard lock(mutex_);
  auto it = endpoints_.find(peer_relay_user_id);
  if (it == endpoints_.end()) {
    EndpointState state;
    state.info = std::move(info);
    state.last_touch = std::chrono::steady_clock::now();
    endpoints_.emplace(peer_relay_user_id, std::move(state));
  } else {
    const bool was_warm = it->second.warm;
    it->second.info = std::move(info);
    it->second.last_touch = std::chrono::steady_clock::now();
    it->second.warm = was_warm;
  }
  return {};
}

bool PeerSessionManager::IsDialable(const std::string& peer_relay_user_id) const {
  std::lock_guard lock(mutex_);
  const auto it = endpoints_.find(peer_relay_user_id);
  if (it == endpoints_.end()) {
    return false;
  }
  const auto now = std::chrono::steady_clock::now();
  if (it->second.dial_failed_until > now) {
    return false;
  }
  return it->second.info && !it->second.info->addresses.empty();
}

bool PeerSessionManager::IsConnected(const std::string& peer_relay_user_id) const {
  std::optional<libp2p::peer::PeerInfo> info;
  {
    std::lock_guard lock(mutex_);
    const auto it = endpoints_.find(peer_relay_user_id);
    if (it == endpoints_.end()) {
      return false;
    }
    info = it->second.info;
  }
  if (!host_.IsRunning() || !info) {
    return false;
  }
  return host_.GetHost().connectedness(*info) == libp2p::Host::Connectedness::CONNECTED;
}

std::optional<libp2p::peer::PeerInfo> PeerSessionManager::ResolvePeerInfo(
    const std::string& peer_relay_user_id) const {
  std::lock_guard lock(mutex_);
  const auto it = endpoints_.find(peer_relay_user_id);
  if (it == endpoints_.end()) {
    return std::nullopt;
  }
  return it->second.info;
}

void PeerSessionManager::MarkWarm(const std::string& peer_relay_user_id) {
  std::lock_guard lock(mutex_);
  auto it = endpoints_.find(peer_relay_user_id);
  if (it == endpoints_.end()) {
    return;
  }
  it->second.warm = true;
  it->second.last_touch = std::chrono::steady_clock::now();
}

void PeerSessionManager::ClearWarm(const std::string& peer_relay_user_id) {
  std::lock_guard lock(mutex_);
  auto it = endpoints_.find(peer_relay_user_id);
  if (it == endpoints_.end()) {
    return;
  }
  it->second.warm = false;
}

void PeerSessionManager::ClearAllWarm() {
  std::lock_guard lock(mutex_);
  for (auto& [_, state] : endpoints_) {
    state.warm = false;
  }
}

void PeerSessionManager::TouchPeerLocked(const std::string& peer_relay_user_id) {
  auto it = endpoints_.find(peer_relay_user_id);
  if (it != endpoints_.end()) {
    it->second.last_touch = std::chrono::steady_clock::now();
  }
}

void PeerSessionManager::DisconnectPeer(const libp2p::peer::PeerId& peer_id) {
  if (!host_.IsRunning()) {
    return;
  }
  host_.Post([this, peer_id]() { host_.GetHost().disconnect(peer_id); });
}

void PeerSessionManager::EvictIfOverCapLocked() {
  if (!host_.IsRunning()) {
    return;
  }
  auto& cmgr = host_.GetHost().getNetwork().getConnectionManager();
  const auto connections = cmgr.getConnections();
  if (connections.size() <= config_.max_connections) {
    return;
  }

  std::vector<std::pair<std::chrono::steady_clock::time_point, std::string>> cold;
  cold.reserve(endpoints_.size());
  for (const auto& [relay_id, state] : endpoints_) {
    if (state.warm) {
      continue;
    }
    cold.emplace_back(state.last_touch, relay_id);
  }
  std::sort(cold.begin(), cold.end());

  size_t to_drop = connections.size() - config_.max_connections;
  for (const auto& [_, relay_id] : cold) {
    if (to_drop == 0) {
      break;
    }
    auto it = endpoints_.find(relay_id);
    if (it == endpoints_.end() || !it->second.info) {
      continue;
    }
    const auto peer_id = it->second.info->id;
    // Disconnect asynchronously; do not nest host ops under assumptions about lock.
    host_.Post([this, peer_id]() { host_.GetHost().disconnect(peer_id); });
    --to_drop;
  }
}

void PeerSessionManager::FinishDial(const std::string& peer_relay_user_id, Roe<void> result) {
  std::vector<DialWaiter> waiters;
  {
    std::lock_guard lock(mutex_);
    auto it = inflight_dials_.find(peer_relay_user_id);
    if (it != inflight_dials_.end()) {
      waiters = std::move(it->second);
      inflight_dials_.erase(it);
    }
    if (!result) {
      auto ep = endpoints_.find(peer_relay_user_id);
      if (ep != endpoints_.end()) {
        ep->second.dial_failed_until = std::chrono::steady_clock::now() + config_.dial_failure_backoff;
      }
    } else {
      TouchPeerLocked(peer_relay_user_id);
      EvictIfOverCapLocked();
    }
  }
  if (concurrent_dials_.load() > 0) {
    concurrent_dials_.fetch_sub(1);
  }
  for (auto& waiter : waiters) {
    if (waiter.on_complete) {
      waiter.on_complete(result);
    }
  }
}

void PeerSessionManager::EnsureConnectionOnIo(const std::string& peer_relay_user_id,
                                              std::function<void(Roe<void>)> on_complete) {
  std::optional<libp2p::peer::PeerInfo> info;
  std::vector<DialWaiter> rejected;
  bool start_dial = false;
  {
    std::lock_guard lock(mutex_);
    auto it = endpoints_.find(peer_relay_user_id);
    if (it == endpoints_.end()) {
      if (on_complete) {
        on_complete(Error("Peer endpoint not registered"));
      }
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (it->second.dial_failed_until > now) {
      if (on_complete) {
        on_complete(Error("Peer dial in backoff"));
      }
      return;
    }
    if (!it->second.info) {
      if (on_complete) {
        on_complete(Error("Peer endpoint missing PeerInfo"));
      }
      return;
    }
    info = it->second.info;

    if (host_.GetHost().connectedness(*info) == libp2p::Host::Connectedness::CONNECTED) {
      TouchPeerLocked(peer_relay_user_id);
      if (on_complete) {
        on_complete({});
      }
      return;
    }

    auto& waiters = inflight_dials_[peer_relay_user_id];
    waiters.push_back(DialWaiter{std::move(on_complete)});
    if (waiters.size() > 1) {
      return;
    }
    if (concurrent_dials_ >= config_.max_concurrent_dials) {
      rejected = std::move(waiters);
      inflight_dials_.erase(peer_relay_user_id);
    } else {
      ++concurrent_dials_;
      start_dial = true;
    }
  }

  if (!rejected.empty()) {
    for (auto& waiter : rejected) {
      if (waiter.on_complete) {
        waiter.on_complete(Error("Too many concurrent dials"));
      }
    }
    return;
  }
  if (!start_dial || !info) {
    return;
  }

  host_.GetHost().connect(*info, [this, peer_relay_user_id](libp2p::Host::ConnectionResult result) {
    if (!result) {
      FinishDial(peer_relay_user_id, Error("libp2p dial failed"));
      return;
    }
    FinishDial(peer_relay_user_id, {});
  });
}

void PeerSessionManager::EnsureConnection(const std::string& peer_relay_user_id,
                                          std::function<void(Roe<void>)> on_complete) {
  if (!host_.IsRunning()) {
    if (on_complete) {
      on_complete(Error("libp2p host not running"));
    }
    return;
  }
  host_.Post([this, peer_relay_user_id, on_complete = std::move(on_complete)]() mutable {
    EnsureConnectionOnIo(peer_relay_user_id, std::move(on_complete));
  });
}

void PeerSessionManager::OpenStream(const std::string& peer_relay_user_id, libp2p::StreamProtocols protocols,
                                    StreamCb cb) {
  if (!host_.IsRunning()) {
    if (cb) {
      cb(std::make_error_code(std::errc::not_connected));
    }
    return;
  }

  std::optional<libp2p::peer::PeerInfo> info;
  {
    std::lock_guard lock(mutex_);
    auto it = endpoints_.find(peer_relay_user_id);
    if (it == endpoints_.end()) {
      if (cb) {
        cb(std::make_error_code(std::errc::no_such_device_or_address));
      }
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (it->second.dial_failed_until > now) {
      if (cb) {
        cb(std::make_error_code(std::errc::host_unreachable));
      }
      return;
    }
    if (!it->second.info) {
      if (cb) {
        cb(std::make_error_code(std::errc::no_such_device_or_address));
      }
      return;
    }
    info = it->second.info;
    TouchPeerLocked(peer_relay_user_id);
  }

  if (!info) {
    if (cb) {
      cb(std::make_error_code(std::errc::no_such_device_or_address));
    }
    return;
  }

  host_.Post([this, peer_relay_user_id, info = *info, protocols = std::move(protocols),
              cb = std::move(cb)]() mutable {
    {
      std::lock_guard lock(mutex_);
      EvictIfOverCapLocked();
    }
    host_.GetHost().newStream(info, std::move(protocols),
                              [this, peer_relay_user_id, cb = std::move(cb)](
                                  libp2p::StreamAndProtocolOrError stream_res) mutable {
                                {
                                  std::lock_guard lock(mutex_);
                                  if (!stream_res) {
                                    auto ep = endpoints_.find(peer_relay_user_id);
                                    if (ep != endpoints_.end()) {
                                      ep->second.dial_failed_until =
                                          std::chrono::steady_clock::now() + config_.dial_failure_backoff;
                                    }
                                  } else {
                                    TouchPeerLocked(peer_relay_user_id);
                                  }
                                }
                                if (cb) {
                                  cb(std::move(stream_res));
                                }
                              });
  });
}

void PeerSessionManager::SweepIdle() {
  if (!host_.IsRunning()) {
    return;
  }
  host_.Post([this]() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<libp2p::peer::PeerId> to_disconnect;
    {
      std::lock_guard lock(mutex_);
      for (auto& [relay_id, state] : endpoints_) {
        if (state.warm) {
          continue;
        }
        if (now - state.last_touch < config_.idle_ttl) {
          continue;
        }
        if (!state.info) {
          continue;
        }
        if (host_.GetHost().connectedness(*state.info) == libp2p::Host::Connectedness::CONNECTED) {
          to_disconnect.push_back(state.info->id);
        }
      }
    }
    for (const auto& peer_id : to_disconnect) {
      host_.GetHost().disconnect(peer_id);
    }
  });
}

void PeerSessionManager::SuspendColdPeers() {
  if (!host_.IsRunning()) {
    return;
  }
  host_.Post([this]() {
    std::vector<libp2p::peer::PeerId> to_disconnect;
    {
      std::lock_guard lock(mutex_);
      for (const auto& [_, state] : endpoints_) {
        if (state.warm || !state.info) {
          continue;
        }
        to_disconnect.push_back(state.info->id);
      }
    }
    for (const auto& peer_id : to_disconnect) {
      host_.GetHost().disconnect(peer_id);
    }
  });
}

void PeerSessionManager::Tick() {
  const auto now = std::chrono::steady_clock::now();
  if (now - last_sweep_ < std::chrono::seconds(15)) {
    return;
  }
  last_sweep_ = now;
  SweepIdle();
}

size_t PeerSessionManager::RegisteredEndpointCount() const {
  std::lock_guard lock(mutex_);
  return endpoints_.size();
}

size_t PeerSessionManager::WarmPeerCount() const {
  std::lock_guard lock(mutex_);
  size_t count = 0;
  for (const auto& [_, state] : endpoints_) {
    if (state.warm) {
      ++count;
    }
  }
  return count;
}

} // namespace pbr
