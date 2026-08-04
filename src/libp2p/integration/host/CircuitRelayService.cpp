#include "libp2p/integration/host/CircuitRelayService.h"

#include "libp2p/integration/host/Libp2pWorker.h"
#include "libp2p/integration/host/StreamFrameIo.h"

#include "base/people/RelayScope.h"
#include "libp2p/integration/host/CircuitBridgeTarget.h"
#include "libp2p/integration/host/StreamJsonFrame.h"

#include <libp2p/basic/read.hpp>
#include <libp2p/basic/write.hpp>
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

using libp2p::Bytes;
using libp2p::connection::Stream;
using libp2p::peer::ProtocolName;

Roe<std::vector<uint8_t>> ReadExactFrame(const std::shared_ptr<Stream>& stream) {
  Bytes header(8);
  std::promise<outcome::result<void>> header_promise;
  auto header_future = header_promise.get_future();
  libp2p::read(stream, header, [&](outcome::result<void> result) { header_promise.set_value(result); });
  if (!header_future.get()) {
    return Error("Failed to read circuit-relay frame header");
  }
  std::vector<uint8_t> frame(header.begin(), header.end());
  uint64_t payload_len = 0;
  for (size_t i = 0; i < 8; ++i) {
    payload_len = (payload_len << 8) | frame[i];
  }
  if (payload_len > kMaxStreamJsonFrameBytes) {
    return Error("circuit-relay frame too large");
  }
  Bytes payload(payload_len);
  std::promise<outcome::result<void>> body_promise;
  auto body_future = body_promise.get_future();
  libp2p::read(stream, payload, [&](outcome::result<void> result) { body_promise.set_value(result); });
  if (!body_future.get()) {
    return Error("Failed to read circuit-relay frame body");
  }
  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

Roe<void> WriteExactFrame(const std::shared_ptr<Stream>& stream, const std::vector<uint8_t>& frame) {
  std::promise<outcome::result<void>> write_promise;
  auto write_future = write_promise.get_future();
  // Yamux WriteQueue stores BytesIn (span) — never pass a temporary Bytes(...).
  libp2p::write(stream, frame, [&](outcome::result<void> result) { write_promise.set_value(result); });
  if (!write_future.get()) {
    return Error("Failed to write circuit-relay frame");
  }
  return {};
}

} // namespace

struct CircuitRelayService::Impl {
  std::mutex handler_mutex;
  Libp2pHost* host = nullptr;
  PeerSessionManager* sessions = nullptr;
  CircuitRelayAdmissionPolicy admission;

  struct ActiveBridgeSession {
    std::shared_ptr<std::atomic<bool>> cancelled;
    std::shared_ptr<StreamBridge> to_target;
    std::shared_ptr<StreamBridge> to_client;
  };
  std::mutex bridges_mu;
  std::vector<std::shared_ptr<ActiveBridgeSession>> active_bridges;

  struct InflightBridge {
    std::shared_ptr<std::atomic<bool>> settled;
    std::shared_ptr<std::promise<Roe<CircuitRelayBridgeResult>>> promise;
  };
  std::mutex bridge_mu;
  std::vector<InflightBridge> inflight_bridges;

  void AbortInflightLocked() {
    for (auto& entry : inflight_bridges) {
      if (entry.settled && !entry.settled->exchange(true) && entry.promise) {
        try {
          entry.promise->set_value(Error("circuit-relay aborted"));
        } catch (const std::future_error&) {
        }
      }
    }
    inflight_bridges.clear();
  }

  void CancelAllBridgesLocked() {
    for (auto& session : active_bridges) {
      if (session && session->cancelled) {
        session->cancelled->store(true, std::memory_order_release);
      }
    }
    active_bridges.clear();
  }

  void StartBridgeSession(const std::shared_ptr<Stream>& client, const std::shared_ptr<Stream>& target) {
    if (!host) {
      return;
    }
    auto session = std::make_shared<ActiveBridgeSession>();
    session->cancelled = std::make_shared<std::atomic<bool>>(false);
    const auto cancel_check = [cancelled = session->cancelled]() {
      return cancelled->load(std::memory_order_acquire);
    };
    host->Post([this, session, client, target, cancel_check]() {
      session->to_target = std::make_shared<StreamBridge>();
      session->to_client = std::make_shared<StreamBridge>();
      session->to_target->Start(client, target, cancel_check, [] {});
      session->to_client->Start(target, client, cancel_check, [] {});
      std::lock_guard lock(bridges_mu);
      active_bridges.push_back(session);
    });
  }

  CircuitRelayBridgeResult RelayBridge(const nlohmann::json& root, const std::shared_ptr<Stream>& client_stream) {
    CircuitRelayBridgeResult out;
    if (root.value("op", "") != "bridge") {
      out.error = "unsupported op";
      return out;
    }
    if (!host || !sessions) {
      out.error = "circuit-relay service not ready";
      return out;
    }

    std::string remote;
    if (auto peer = client_stream->remotePeerId()) {
      remote = peer.value().toBase58();
    }
    CircuitRelayAdmissionPolicy policy;
    {
      std::lock_guard lock(handler_mutex);
      policy = admission;
    }
    if (!RelayAdmissionAllowsDialer(policy.serve_scope_mask, remote, policy.contact_peer_ids)) {
      out.error = "relay scope: stranger refused";
      return out;
    }

    CircuitBridgeTarget target;
    target.target_peer_id = root.value("target_peer_id", "");
    target.target_multiaddr = root.value("target_multiaddr", "");
    target.target_protocol = root.value("target_protocol", "");

    auto normalized = NormalizeCircuitBridgeTarget(*sessions, *host, target);
    if (!normalized) {
      out.error = normalized.error().message;
      return out;
    }
    const std::string& target_peer_id = normalized->first;
    const std::string& resolved_multiaddr = normalized->second;
    out.resolved_multiaddr = resolved_multiaddr;

    const std::string target_key = target_peer_id;
    if (auto registered = sessions->RegisterEndpoint(target_key, resolved_multiaddr); !registered) {
      out.error = registered.error().message;
      return out;
    }
    sessions->ClearDialBackoff(target_key);

    const int timeout_ms = root.value("timeout_ms", 8000);
    const auto wait_for = std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 8000);

    auto dial_promise = std::make_shared<std::promise<Roe<void>>>();
    auto dial_future = dial_promise->get_future();
    sessions->EnsureConnection(target_key, [dial_promise](Roe<void> result) {
      try {
        dial_promise->set_value(std::move(result));
      } catch (const std::future_error&) {
      }
    });
    if (dial_future.wait_for(wait_for) != std::future_status::ready) {
      out.error = "relay dial timed out";
      return out;
    }
    auto dialed = dial_future.get();
    if (!dialed) {
      out.error = dialed.error().message;
      return out;
    }

    libp2p::StreamProtocols protocols;
    const std::string target_protocol = root.value("target_protocol", "");
    if (!target_protocol.empty()) {
      protocols.push_back(ProtocolName{target_protocol});
    } else {
      protocols.push_back(ProtocolName{kCircuitRelayProtocolId});
    }

    auto stream_promise = std::make_shared<std::promise<libp2p::StreamAndProtocolOrError>>();
    auto stream_future = stream_promise->get_future();
    sessions->OpenStream(target_key, protocols,
                        [stream_promise](libp2p::StreamAndProtocolOrError res) {
                          try {
                            stream_promise->set_value(std::move(res));
                          } catch (const std::future_error&) {
                          }
                        });
    if (stream_future.wait_for(wait_for) != std::future_status::ready) {
      out.error = "relay target stream timed out";
      return out;
    }
    auto target_stream_res = stream_future.get();
    if (!target_stream_res) {
      out.error = "relay target stream failed";
      return out;
    }

    nlohmann::json response = {{"v", 1}, {"ok", true}, {"resolved_multiaddr", resolved_multiaddr}};
    if (auto encoded = EncodeStreamJsonFrame(response.dump())) {
      if (!WriteExactFrame(client_stream, *encoded)) {
        out.error = "failed to ack bridge";
        return out;
      }
    }

    StartBridgeSession(client_stream, target_stream_res.value().stream);
    out.ok = true;
    return out;
  }

  void HandleStream(libp2p::StreamAndProtocol stream_and_protocol) {
    auto stream = std::move(stream_and_protocol.stream);
    Libp2pHost* host_for_post = nullptr;
    {
      std::lock_guard<std::mutex> lock(handler_mutex);
      host_for_post = host;
    }
    if (!host_for_post) {
      return;
    }
    PostLibp2pWorker(*host_for_post, WorkerLane::Normal, [this, stream = std::move(stream)]() mutable {
      CircuitRelayBridgeResult result;
      auto frame = ReadExactFrame(stream);
      if (!frame) {
        result.error = frame.error().message;
      } else if (auto json_utf8 = DecodeStreamJsonFrame(*frame)) {
        nlohmann::json root = nlohmann::json::parse(*json_utf8, nullptr, false);
        if (root.is_discarded() || !root.is_object()) {
          result.error = "invalid circuit-relay json";
        } else {
          result = RelayBridge(root, stream);
          if (result.ok) {
            return;
          }
        }
      } else {
        result.error = json_utf8.error().message;
      }

      nlohmann::json response = {{"v", 1}, {"ok", false}, {"error", result.error}};
      if (auto encoded = EncodeStreamJsonFrame(response.dump())) {
        (void)WriteExactFrame(stream, *encoded);
      }
      stream->close([](auto&&) {});
    });
  }
};

CircuitRelayService::CircuitRelayService(Libp2pHost& host, PeerSessionManager& sessions)
    : impl_(std::make_unique<Impl>()), host_(host), sessions_(sessions) {
  impl_->host = &host_;
  impl_->sessions = &sessions_;
}

CircuitRelayService::~CircuitRelayService() {
  Stop();
}

void CircuitRelayService::Start() {
  if (started_ || !host_.IsRunning()) {
    return;
  }
  started_ = true;
  host_.GetHost().setProtocolHandler({ProtocolName{kCircuitRelayProtocolId}},
                                     [impl = impl_.get()](libp2p::StreamAndProtocol stream) {
                                       impl->HandleStream(std::move(stream));
                                     });
}

void CircuitRelayService::Stop() {
  started_ = false;
  AbortInflightRequests();
  std::lock_guard lock(impl_->bridges_mu);
  impl_->CancelAllBridgesLocked();
}

void CircuitRelayService::AbortInflightRequests() {
  std::lock_guard lock(impl_->bridge_mu);
  impl_->AbortInflightLocked();
}

void CircuitRelayService::SetAdmissionPolicy(CircuitRelayAdmissionPolicy policy) {
  std::lock_guard<std::mutex> lock(impl_->handler_mutex);
  impl_->admission = std::move(policy);
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

  auto frame = EncodeStreamJsonFrame(request.dump());
  if (!frame) {
    return frame.error();
  }

  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto result_promise = std::make_shared<std::promise<Roe<CircuitRelayBridgeResult>>>();
  auto result_future = result_promise->get_future();
  {
    std::lock_guard lock(impl_->bridge_mu);
    impl_->inflight_bridges.push_back(Impl::InflightBridge{settled, result_promise});
  }

  auto finish = [settled, result_promise](Roe<CircuitRelayBridgeResult> value) {
    if (!settled->exchange(true)) {
      try {
        result_promise->set_value(std::move(value));
      } catch (const std::future_error&) {
      }
    }
  };

  sessions_.OpenStream(relay_peer_key, {ProtocolName{kCircuitRelayProtocolId}},
                       [&host = host_, frame = *frame, settled, finish](libp2p::StreamAndProtocolOrError stream_res) {
                         if (settled->load(std::memory_order_acquire)) {
                           return;
                         }
                         PostLibp2pWorker(host, WorkerLane::Normal,
                                          [frame, settled, finish, stream_res = std::move(stream_res)]() mutable {
                                            if (settled->load(std::memory_order_acquire)) {
                                              return;
                                            }
                                            if (!stream_res) {
                                              finish(Error("circuit-relay stream open failed"));
                                              return;
                                            }
                                            auto stream = std::move(stream_res.value().stream);
                                            if (!WriteExactFrame(stream, frame)) {
                                              finish(Error("Failed to send circuit-relay request"));
                                              return;
                                            }
                                            if (settled->load(std::memory_order_acquire)) {
                                              stream->close([](auto&&) {});
                                              return;
                                            }
                                            auto response_frame = ReadExactFrame(stream);
                                            if (settled->load(std::memory_order_acquire)) {
                                              stream->close([](auto&&) {});
                                              return;
                                            }
                                            if (!response_frame) {
                                              finish(Error("Failed to read circuit-relay response"));
                                              stream->close([](auto&&) {});
                                              return;
                                            }
                                            auto json_utf8 = DecodeStreamJsonFrame(*response_frame);
                                            if (!json_utf8) {
                                              finish(json_utf8.error());
                                              stream->close([](auto&&) {});
                                              return;
                                            }
                                            nlohmann::json root = nlohmann::json::parse(*json_utf8, nullptr, false);
                                            if (root.is_discarded() || !root.is_object()) {
                                              finish(Error("invalid circuit-relay response"));
                                              stream->close([](auto&&) {});
                                              return;
                                            }
                                            CircuitRelayBridgeResult parsed;
                                            parsed.ok = root.value("ok", false);
                                            parsed.error = root.value("error", "");
                                            parsed.resolved_multiaddr = root.value("resolved_multiaddr", "");
                                            if (!parsed.ok) {
                                              stream->close([](auto&&) {});
                                              finish(parsed);
                                              return;
                                            }
                                            parsed.stream = stream;
                                            finish(parsed);
                                          });
                       });

  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
  for (;;) {
    const auto status = result_future.wait_for(std::chrono::milliseconds(50));
    if (status == std::future_status::ready) {
      std::lock_guard lock(impl_->bridge_mu);
      impl_->inflight_bridges.erase(
          std::remove_if(impl_->inflight_bridges.begin(), impl_->inflight_bridges.end(),
                         [&](const Impl::InflightBridge& e) { return e.settled == settled; }),
          impl_->inflight_bridges.end());
      return result_future.get();
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      finish(Error("circuit-relay bridge timed out"));
      std::lock_guard lock(impl_->bridge_mu);
      impl_->inflight_bridges.erase(
          std::remove_if(impl_->inflight_bridges.begin(), impl_->inflight_bridges.end(),
                         [&](const Impl::InflightBridge& e) { return e.settled == settled; }),
          impl_->inflight_bridges.end());
      return result_future.get();
    }
  }
}

} // namespace pbr
