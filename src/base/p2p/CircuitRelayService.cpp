#include "base/p2p/CircuitRelayService.h"
#include "base/p2p/CircuitRelayRuntime.h"

#include "base/p2p/Libp2pWorker.h"
#include "base/p2p/SettledWait.h"
#include "base/p2p/StreamFrameIo.h"

#include "base/people/RelayScope.h"
#include "base/p2p/CircuitBridgeTarget.h"
#include "base/p2p/StreamJsonFrame.h"

#include <libp2p/connection/stream.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/peer/protocol.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <vector>

namespace pbr {

namespace {

using libp2p::connection::Stream;
using libp2p::peer::ProtocolName;

} // namespace



CircuitRelayService::CircuitRelayService(Libp2pHost& host, PeerSessionManager& sessions)
    : runtime_(std::make_shared<CircuitRelayRuntime>()), host_(host), sessions_(sessions) {
  runtime_->host = &host_;
  runtime_->sessions = &sessions_;
}

CircuitRelayService::~CircuitRelayService() {
  Stop();
}

void CircuitRelayService::Start() {
  if (started_ || !host_.IsRunning()) {
    return;
  }
  started_ = true;
  runtime_->stopping.store(false, std::memory_order_release);
  host_.GetHost().setProtocolHandler({ProtocolName{kCircuitRelayProtocolId}},
                                     [runtime = runtime_](libp2p::StreamAndProtocol stream) {
                                       runtime->HandleStream(std::move(stream));
                                     });
}

void CircuitRelayService::Stop() {
  started_ = false;
  if (runtime_) {
    runtime_->stopping.store(true, std::memory_order_release);
  }
  AbortInflightRequests();
  std::lock_guard lock(runtime_->bridges_mu);
  runtime_->CancelAllBridgesLocked();
}

CircuitRelayRuntimeStats CircuitRelayService::RuntimeStats() const {
  CircuitRelayRuntimeStats out;
  if (!started_ || !runtime_) {
    return out;
  }
  std::lock_guard lock(runtime_->bridges_mu);
  for (const auto& entry : runtime_->active_bridges) {
    if (entry && entry->cancelled && !entry->cancelled->load(std::memory_order_acquire)) {
      ++out.active_bridges;
    }
  }
  return out;
}

void CircuitRelayService::AbortInflightRequests() {
  std::lock_guard lock(runtime_->bridge_mu);
  runtime_->AbortInflightLocked();
}

void CircuitRelayService::SetAdmissionPolicy(CircuitRelayAdmissionPolicy policy) {
  std::lock_guard<std::mutex> lock(runtime_->handler_mutex);
  runtime_->admission = std::move(policy);
}

Roe<CircuitRelayBridgeResult> CircuitRelayService::RequestBridge(const std::string& relay_peer_key,
                                                                 const std::string& target_multiaddr,
                                                                 int timeout_ms) {
  CircuitBridgeTarget target;
  target.target_multiaddr = target_multiaddr;
  return RequestBridge(relay_peer_key, target, timeout_ms);
}

Roe<CircuitRelayBridgeResult> CircuitRelayService::RequestBridge(const std::string& relay_peer_key,
                                                                 const CircuitBridgeTarget& target_in,
                                                                 int timeout_ms) {
  if (!host_.IsRunning()) {
    return Error("circuit-relay host not running");
  }
  if (!sessions_.IsDialable(relay_peer_key)) {
    return Error("relay peer endpoint not registered");
  }

  CircuitBridgeTarget target = target_in;
  if (target.target_multiaddr.empty() && target.target_peer_id.empty()) {
    return Error("missing circuit bridge target");
  }

  if (target.target_multiaddr.empty()) {
    if (auto resolved = ResolveCircuitTargetMultiaddr(sessions_, host_, target.target_peer_id)) {
      target.target_multiaddr = *resolved;
    }
  }

  nlohmann::json request = {{"v", 1},
                            {"op", "bridge"},
                            {"timeout_ms", timeout_ms > 0 ? timeout_ms : 8000}};
  if (!target.target_peer_id.empty()) {
    request["target_peer_id"] = target.target_peer_id;
  }
  if (!target.target_multiaddr.empty()) {
    request["target_multiaddr"] = target.target_multiaddr;
  }
  if (!target.target_protocol.empty()) {
    request["target_protocol"] = target.target_protocol;
  }

  SettledWait<CircuitRelayBridgeResult> wait;
  {
    std::lock_guard lock(runtime_->bridge_mu);
    runtime_->inflight_bridges.push_back(CircuitRelayRuntime::InflightBridge{wait, {}});
  }

  sessions_.OpenStream(relay_peer_key, {ProtocolName{kCircuitRelayProtocolId}},
                       [runtime = runtime_, json = request.dump(), wait](
                           libp2p::StreamAndProtocolOrError stream_res) {
                         if (wait.IsSettled()) {
                           if (stream_res) {
                             stream_res.value().stream->reset();
                           }
                           return;
                         }
                         Libp2pHost* host = nullptr;
                         {
                           std::lock_guard lock(runtime->handler_mutex);
                           host = runtime->host;
                         }
                         if (!host) {
                           wait.Finish(Error("circuit-relay service not ready"));
                           return;
                         }
                         PostLibp2pWorker(*host, WorkerLane::Normal,
                                          [runtime, json, wait,
                                           stream_res = std::move(stream_res)]() mutable {
                                            runtime->RunClientBridgeOnWorker(json, wait,
                                                                             std::move(stream_res));
                                          });
                       });

  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  auto result = wait.Wait(std::chrono::milliseconds(wait_ms), Error("circuit-relay bridge timed out"));
  std::lock_guard lock(runtime_->bridge_mu);
  runtime_->inflight_bridges.erase(
      std::remove_if(runtime_->inflight_bridges.begin(), runtime_->inflight_bridges.end(),
                     [&](const CircuitRelayRuntime::InflightBridge& e) { return e.wait.SameAs(wait); }),
      runtime_->inflight_bridges.end());
  return result;
}

} // namespace pbr
