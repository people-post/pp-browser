#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "base/p2p/Libp2pHost.h"
#include "base/p2p/PeerAddressBook.h"

#include <libp2p/connection/stream.hpp>
#include <libp2p/connection/stream_and_protocol.hpp>
#include <libp2p/event/bus.hpp>
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

class CircuitRelayService;

struct PeerSessionConfig {
  size_t max_connections = 48;
  size_t max_concurrent_dials = 6;
  std::chrono::milliseconds dial_timeout{8000};
  std::chrono::milliseconds idle_ttl{180000};
  std::chrono::milliseconds dial_failure_backoff{30000};
};

/** Live peer link phase for UI (on-demand dial + warm session). */
enum class PeerLinkPhase {
  Unavailable, // host down / no endpoint
  Idle,        // dialable, not connected
  Dialing,
  Connected,
  Backoff, // recent dial failure; wait before retry
};

struct PeerLinkSnapshot {
  PeerLinkPhase phase = PeerLinkPhase::Unavailable;
  std::chrono::milliseconds backoff_remaining{0};
  std::string detail; // last dial error (technical); prefer PeerDialErrorUserCopy for display
  bool host_running = false;
  bool has_endpoint = false;
};

/** Map dial/transport Error::message strings to end-user copy. */
std::string PeerDialErrorUserCopy(const std::string& technical_message);

/**
 * On-demand dial + warm-active session policy over a shared Libp2pHost.
 * Does not pool sockets separately — reuses ConnectionManager via Host::newStream/connect.
 */
class PeerSessionManager : public Module {
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
  /**
   * Direct dial path or an established protocol-scoped circuit hop.
   * Use before OpenStream for call-media / media_relay so circuit-only peers are eligible.
   */
  bool IsReachableForProtocol(const std::string& peer_relay_user_id,
                              const std::string& target_protocol) const;
  bool IsConnected(const std::string& peer_relay_user_id) const;
  bool IsDialing(const std::string& peer_relay_user_id) const;
  PeerLinkSnapshot GetLinkSnapshot(const std::string& peer_relay_user_id) const;

  std::optional<libp2p::peer::PeerInfo> ResolvePeerInfo(const std::string& peer_relay_user_id) const;

  /** Best dial multiaddr for a peer key (endpoint, then L1 address book). */
  std::optional<std::string> PreferredPeerMultiaddr(const std::string& peer_relay_user_id) const;

  /** L2: refresh book/endpoints after remote Identify completes. */
  void NoteRemoteIdentify(const std::string& peer_id_base58);

  /** L2: upsert a book entry (e.g. self advertised addrs). */
  Roe<void> UpsertBookEntry(const std::string& peer_id_base58, const std::string& multiaddr,
                           PeerAddrSource source);

  /**
   * L3: try circuit bridge via candidate relays when direct dial is unavailable.
   * `target_protocol` is the stream protocol opened on the remote peer after bridge (e.g. call-media).
   */
  Roe<void> TryEnsureHopViaCircuit(const std::string& target_peer_id, CircuitRelayService& circuit,
                                   const std::vector<std::string>& relay_peer_ids,
                                   const std::string& target_protocol, int timeout_ms = 8000,
                                   const std::string& target_multiaddr = {});

  /**
   * Record a successful circuit bridge so OpenStream reuses the bridged stream.
   * Does not register a direct dial path to the target (A↛B except via relay).
   */
  Roe<void> InstallCircuitHop(const std::string& target_peer_id, const std::string& relay_peer_id,
                              const std::string& target_protocol,
                              std::shared_ptr<libp2p::connection::Stream> stream);

  bool IsCircuitBacked(const std::string& peer_relay_user_id) const;
  bool IsCircuitBacked(const std::string& peer_relay_user_id, const std::string& target_protocol) const;
  void ClearCircuitHop(const std::string& peer_relay_user_id);
  void ClearCircuitHop(const std::string& peer_relay_user_id, const std::string& target_protocol);

  /** Composite storage key for protocol-scoped circuit hops. */
  static std::string CircuitHopKey(const std::string& peer_key, const std::string& target_protocol);

  /** Mark peer as warm (kept across idle eviction / background suspend of cold peers). */
  void MarkWarm(const std::string& peer_relay_user_id);
  void ClearWarm(const std::string& peer_relay_user_id);
  void ClearAllWarm();

  /** Clear dial failure backoff so the next EnsureConnection may dial immediately. */
  void ClearDialBackoff(const std::string& peer_relay_user_id);
  /** Fail waiters and drop inflight dial tracking (stuck host.connect / retry). */
  void AbortInflightDial(const std::string& peer_relay_user_id);

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
    std::string last_error;
    bool warm = false;
  };

  struct DialWaiter {
    std::function<void(Roe<void>)> on_complete;
  };

  struct CircuitHopLink {
    std::shared_ptr<libp2p::connection::Stream> stream;
    std::string relay_peer_id;
    std::string target_protocol;
  };

  std::optional<CircuitHopLink> FindCircuitHopLocked(const std::string& peer_relay_user_id,
                                                     const std::string& target_protocol) const;
  void StoreCircuitHopLocked(const std::string& peer_relay_user_id, CircuitHopLink link);
  bool HasAnyCircuitHopLocked(const std::string& peer_relay_user_id) const;
  bool HasDirectDialPathLocked(const std::string& peer_relay_user_id) const;

  void TouchPeerLocked(const std::string& peer_relay_user_id);
  void EvictIfOverCapLocked();
  void DisconnectPeer(const libp2p::peer::PeerId& peer_id);
  void FinishDial(const std::string& peer_relay_user_id, Roe<void> result);
  void EnsureConnectionOnIo(const std::string& peer_relay_user_id, std::function<void(Roe<void>)> on_complete);
  void InstallConnectionHandler();
  void OnInboundConnection(libp2p::peer::PeerInfo info);
  void OnNewStreamResult(const std::string& peer_relay_user_id, const std::string& proto_log,
                         StreamCb cb, libp2p::StreamAndProtocolOrError stream_res);
  void MaybeHydrateEndpointFromBookLocked(const std::string& peer_relay_user_id);
  PeerAddrSource SourceForEndpointKey(const std::string& peer_relay_user_id) const;
  std::optional<std::string> PeerIdBase58ForKeyLocked(const std::string& peer_relay_user_id) const;

  Libp2pHost& host_;
  PeerSessionConfig config_;
  PeerAddressBook address_book_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, EndpointState> endpoints_;
  std::unordered_map<std::string, std::vector<DialWaiter>> inflight_dials_;
  std::unordered_map<std::string, CircuitHopLink> circuit_hops_;
  std::atomic<size_t> concurrent_dials_{0};
  std::chrono::steady_clock::time_point last_sweep_{};
  std::optional<libp2p::event::Handle> connection_handler_;
};

} // namespace pbr
