#pragma once

#include "base/mesh/channel/ChannelSession.h"
#include "base/mesh/link/MeshRuntime.h"
#include "base/p2p/CircuitBridgeTarget.h"
#include "base/p2p/CircuitRelayService.h"
#include "base/people/RelayScope.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <functional>
#include <memory>
#include <string>

namespace pbr {

struct AmpCircuitRelayBridgeResult {
  bool ok = false;
  std::string error;
  std::string resolved_multiaddr;
  /** Client tunnel channel after successful bridge (may be null). */
  std::shared_ptr<amp::ChannelSession> session;
};

/**
 * `/pp-browser/circuit-relay/1.0.0` over AMP ChannelSession (MeshRuntime).
 * Parallel stack for migration — production still uses CircuitRelayService ([A020]).
 * v1 tunnel = L4-opaque DATA splice on hop Sessions (parity with StreamBridge).
 */
class AmpCircuitRelayService {
public:
  using IoPump = std::function<void()>;
  using FrameHandler = amp::ChannelSession::FrameHandler;
  using ClosedCallback = amp::ChannelSession::ClosedCallback;

  AmpCircuitRelayService(amp::MeshRuntime& runtime, IoPump io_pump = {});
  ~AmpCircuitRelayService();

  AmpCircuitRelayService(const AmpCircuitRelayService&) = delete;
  AmpCircuitRelayService& operator=(const AmpCircuitRelayService&) = delete;

  void Start();
  void Stop();
  bool IsStarted() const { return started_; }

  void SetAdmissionPolicy(CircuitRelayAdmissionPolicy policy);

  /** Unblock in-flight RequestBridge waiters. */
  void AbortInflightRequests();

  /**
   * Client: open circuit channel to relay, send bridge JSON, wait for ack.
   * On success `result.session` is ready for opaque DATA; optional `on_payload` receives
   * forwarded frames from the target.
   */
  Roe<AmpCircuitRelayBridgeResult> RequestBridge(const std::string& relay_peer_key,
                                                 const CircuitBridgeTarget& target,
                                                 FrameHandler on_payload = {},
                                                 ClosedCallback on_closed = {}, int timeout_ms = 8000);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  amp::MeshRuntime& runtime_;
  IoPump io_pump_;
  bool started_ = false;
};

} // namespace pbr
