#pragma once

#include "base/p2p/CircuitBridgeTarget.h"
#include "base/p2p/CircuitRelayTypes.h"
#include "base/p2p/RelayRuntimeStats.h"
#include "common/Error.h"
#include "common/PbrCompat.h"

#include <memory>
#include <string>

namespace pbr {

class Libp2pHost;
class PeerSessionManager;
class CircuitRelayRuntime;

/**
 * Legacy TCP circuit-relay service. Product path is CircuitTunnelCoordinator (D10/A017).
 */
class CircuitRelayService {
public:
  CircuitRelayService(Libp2pHost& host, PeerSessionManager& sessions);
  ~CircuitRelayService();

  CircuitRelayService(const CircuitRelayService&) = delete;
  CircuitRelayService& operator=(const CircuitRelayService&) = delete;

  void Start();
  void Stop();
  bool IsStarted() const { return started_; }
  CircuitRelayRuntimeStats RuntimeStats() const;
  void SetAdmissionPolicy(CircuitRelayAdmissionPolicy policy);
  void AbortInflightRequests();
  Roe<CircuitRelayBridgeResult> RequestBridge(const std::string& relay_peer_key,
                                              const CircuitBridgeTarget& target, int timeout_ms = 8000);
  Roe<CircuitRelayBridgeResult> RequestBridge(const std::string& relay_peer_key,
                                              const std::string& target_multiaddr,
                                              int timeout_ms = 8000);

private:
  std::shared_ptr<CircuitRelayRuntime> runtime_;
  Libp2pHost& host_;
  PeerSessionManager& sessions_;
  bool started_ = false;
};

} // namespace pbr
