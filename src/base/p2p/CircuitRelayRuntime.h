#pragma once

#include "base/p2p/CircuitRelayService.h"
#include "base/p2p/SettledWait.h"
#include "base/p2p/StreamFrameIo.h"

#include <libp2p/connection/stream.hpp>
#include <libp2p/connection/stream_and_protocol.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <vector>

namespace pbr {

/**
 * Shared circuit-relay state machine (inbound bridge + client RequestBridge).
 * Owned by CircuitRelayService via shared_ptr so protocol / OpenStream / worker
 * callbacks cannot UAF across Stop.
 */
class CircuitRelayRuntime : public std::enable_shared_from_this<CircuitRelayRuntime> {
public:
  using Stream = libp2p::connection::Stream;

  std::mutex handler_mutex;
  Libp2pHost* host = nullptr;
  PeerSessionManager* sessions = nullptr;
  CircuitRelayAdmissionPolicy admission;
  std::atomic<bool> stopping{false};

  struct ActiveBridgeSession {
    std::shared_ptr<std::atomic<bool>> cancelled;
    std::shared_ptr<StreamBridge> to_target;
    std::shared_ptr<StreamBridge> to_client;
  };
  std::mutex bridges_mu;
  std::vector<std::shared_ptr<ActiveBridgeSession>> active_bridges;

  struct InflightBridge {
    SettledWait<CircuitRelayBridgeResult> wait;
    std::shared_ptr<Stream> stream;
  };
  std::mutex bridge_mu;
  std::vector<InflightBridge> inflight_bridges;

  void AbortInflightLocked();
  void AttachInflightStream(const SettledWait<CircuitRelayBridgeResult>& wait,
                            const std::shared_ptr<Stream>& stream);
  template <typename T>
  bool WaitReadyOrStop(std::future<T>& future, std::chrono::milliseconds wait_for);
  void CancelAllBridgesLocked();
  void RemoveBridgeSessionLocked(const std::shared_ptr<ActiveBridgeSession>& session);
  void PruneCancelledBridgesLocked();
  void StartBridgeSession(const std::shared_ptr<Stream>& client, const std::shared_ptr<Stream>& target);
  CircuitRelayBridgeResult RelayBridge(const nlohmann::json& root,
                                       const std::shared_ptr<Stream>& client_stream);
  void HandleStream(libp2p::StreamAndProtocol stream_and_protocol);
  void RunClientBridgeOnWorker(const std::string& json, SettledWait<CircuitRelayBridgeResult> wait,
                               libp2p::StreamAndProtocolOrError stream_res);
};

template <typename T>
bool CircuitRelayRuntime::WaitReadyOrStop(std::future<T>& future,
                                          std::chrono::milliseconds wait_for) {
  const auto deadline = std::chrono::steady_clock::now() + wait_for;
  for (;;) {
    if (stopping.load(std::memory_order_acquire)) {
      return false;
    }
    if (future.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
  }
}

} // namespace pbr
