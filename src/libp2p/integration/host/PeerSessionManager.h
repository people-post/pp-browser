#pragma once

#include "common/Error.h"
#include "libp2p/integration/host/Libp2pHost.h"

#include <libp2p/connection/stream_and_protocol.hpp>
#include <libp2p/peer/peer_info.hpp>
#include <libp2p/peer/stream_protocols.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pbr {

struct PeerSessionConfig {
  size_t max_connections = 48;
  size_t max_concurrent_dials = 6;
  std::chrono::milliseconds dial_timeout{8000};
  std::chrono::milliseconds idle_ttl{180000};
  std::chrono::milliseconds dial_failure_backoff{30000};
};

/**
 * On-demand dial + warm-active session policy over a shared Libp2pHost.
 * Does not pool sockets separately — reuses ConnectionManager via Host::newStream/connect.
 */
class PeerSessionManager {
public:
  using StreamCb = libp2p::StreamAndProtocolOrErrorCb;

  PeerSessionManager(Libp2pHost& host, PeerSessionConfig config = {});
  ~PeerSessionManager();

  PeerSessionManager(const PeerSessionManager&) = delete;
  PeerSessionManager& operator=(const PeerSessionManager&) = delete;

  void SetConfig(PeerSessionConfig config);
  const PeerSessionConfig& GetConfig() const { return config_; }

  /** Map relay communicating identity → dialable multiaddr (must include /p2p/). */
  Roe<void> RegisterEndpoint(const std::string& peer_relay_user_id, const std::string& multiaddr);

  bool IsDialable(const std::string& peer_relay_user_id) const;
  bool IsConnected(const std::string& peer_relay_user_id) const;

  std::optional<libp2p::peer::PeerInfo> ResolvePeerInfo(const std::string& peer_relay_user_id) const;

  /** Mark peer as warm (kept across idle eviction / background suspend of cold peers). */
  void MarkWarm(const std::string& peer_relay_user_id);
  void ClearWarm(const std::string& peer_relay_user_id);
  void ClearAllWarm();

  /** Dial if needed; coalesce concurrent dials; enforce caps. */
  void EnsureConnection(const std::string& peer_relay_user_id,
                        std::function<void(Roe<void>)> on_complete = {});

  /** Open a protocol stream (dials/reuses connection under the hood). Touches idle timer. */
  void OpenStream(const std::string& peer_relay_user_id, libp2p::StreamProtocols protocols, StreamCb cb);

  /** Disconnect idle cold peers; keep warm set. */
  void SweepIdle();

  /** Background: drop non-warm connections. */
  void SuspendColdPeers();

  /** Tick from UI/IO poll — runs idle sweep periodically. */
  void Tick();

  size_t RegisteredEndpointCount() const;
  size_t WarmPeerCount() const;

private:
  struct EndpointState {
    std::optional<libp2p::peer::PeerInfo> info;
    std::chrono::steady_clock::time_point last_touch{};
    std::chrono::steady_clock::time_point dial_failed_until{};
    bool warm = false;
  };

  struct DialWaiter {
    std::function<void(Roe<void>)> on_complete;
  };

  void TouchPeerLocked(const std::string& peer_relay_user_id);
  void EvictIfOverCapLocked();
  void DisconnectPeer(const libp2p::peer::PeerId& peer_id);
  void FinishDial(const std::string& peer_relay_user_id, Roe<void> result);
  void EnsureConnectionOnIo(const std::string& peer_relay_user_id, std::function<void(Roe<void>)> on_complete);

  Libp2pHost& host_;
  PeerSessionConfig config_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, EndpointState> endpoints_;
  std::unordered_map<std::string, std::vector<DialWaiter>> inflight_dials_;
  std::atomic<size_t> concurrent_dials_{0};
  std::chrono::steady_clock::time_point last_sweep_{};
};

} // namespace pbr
