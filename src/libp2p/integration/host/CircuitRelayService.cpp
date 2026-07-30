#include "libp2p/integration/host/CircuitRelayService.h"

#include "libp2p/integration/host/StreamJsonFrame.h"

#include <libp2p/basic/read.hpp>
#include <libp2p/basic/write.hpp>
#include <libp2p/connection/stream.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/peer/protocol.hpp>

#include <array>
#include <chrono>
#include <future>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

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
  libp2p::write(stream, libp2p::Bytes(frame), [&](outcome::result<void> result) { write_promise.set_value(result); });
  if (!write_future.get()) {
    return Error("Failed to write circuit-relay frame");
  }
  return {};
}

void BridgeStreams(const std::shared_ptr<Stream>& a, const std::shared_ptr<Stream>& b) {
  auto pump = [](std::shared_ptr<Stream> from, std::shared_ptr<Stream> to) {
    std::thread([from = std::move(from), to = std::move(to)]() mutable {
      while (true) {
        Bytes chunk(16 * 1024);
        std::promise<outcome::result<void>> read_promise;
        auto read_future = read_promise.get_future();
        libp2p::read(from, chunk,
                     [&](outcome::result<void> result) { read_promise.set_value(result); });
        if (!read_future.get()) {
          break;
        }
        std::promise<outcome::result<void>> write_promise;
        auto write_future = write_promise.get_future();
        libp2p::write(to, chunk, [&](outcome::result<void> result) { write_promise.set_value(result); });
        if (!write_future.get()) {
          break;
        }
      }
      from->close([](auto&&) {});
      to->close([](auto&&) {});
    }).detach();
  };
  pump(a, b);
  pump(b, a);
}

CircuitRelayBridgeResult RelayBridge(PeerSessionManager& sessions, const nlohmann::json& root,
                                     const std::shared_ptr<Stream>& client_stream,
                                     const CircuitRelayAdmissionPolicy& admission) {
  CircuitRelayBridgeResult out;
  if (root.value("op", "") != "bridge") {
    out.error = "unsupported op";
    return out;
  }
  if (admission.prefer_contacts_only && !admission.contact_peer_ids.empty()) {
    std::string remote;
    if (auto peer = client_stream->remotePeerId()) {
      remote = peer.value().toBase58();
    }
    if (remote.empty() || admission.contact_peer_ids.find(remote) == admission.contact_peer_ids.end()) {
      out.error = "prefer contacts: stranger refused";
      return out;
    }
  }
  const std::string target = root.value("target_multiaddr", "");
  if (target.empty()) {
    out.error = "missing target_multiaddr";
    return out;
  }

  const std::string target_key = "circuit-relay:" + target;
  if (auto registered = sessions.RegisterEndpoint(target_key, target); !registered) {
    out.error = registered.error().message;
    return out;
  }
  sessions.ClearDialBackoff(target_key);

  const int timeout_ms = root.value("timeout_ms", 8000);
  const auto wait_for = std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 8000);

  std::promise<Roe<void>> dial_promise;
  auto dial_future = dial_promise.get_future();
  sessions.EnsureConnection(target_key, [&](Roe<void> result) { dial_promise.set_value(std::move(result)); });
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

  std::promise<libp2p::StreamAndProtocolOrError> stream_promise;
  auto stream_future = stream_promise.get_future();
  sessions.OpenStream(target_key, protocols,
                      [&](libp2p::StreamAndProtocolOrError res) { stream_promise.set_value(std::move(res)); });
  if (stream_future.wait_for(wait_for) != std::future_status::ready) {
    out.error = "relay target stream timed out";
    return out;
  }
  auto target_stream_res = stream_future.get();
  if (!target_stream_res) {
    out.error = "relay target stream failed";
    return out;
  }

  nlohmann::json response = {{"v", 1}, {"ok", true}};
  if (auto encoded = EncodeStreamJsonFrame(response.dump())) {
    if (!WriteExactFrame(client_stream, *encoded)) {
      out.error = "failed to ack bridge";
      return out;
    }
  }

  BridgeStreams(client_stream, target_stream_res.value().stream);
  out.ok = true;
  return out;
}

} // namespace

struct CircuitRelayService::Impl {
  std::mutex handler_mutex;
  PeerSessionManager* sessions = nullptr;
  CircuitRelayAdmissionPolicy admission;

  void HandleStream(libp2p::StreamAndProtocol stream_and_protocol) {
    auto stream = std::move(stream_and_protocol.stream);
    std::thread([this, stream = std::move(stream)]() mutable {
      CircuitRelayAdmissionPolicy policy;
      {
        std::lock_guard<std::mutex> lock(handler_mutex);
        policy = admission;
      }
      CircuitRelayBridgeResult result;
      auto frame = ReadExactFrame(stream);
      if (!frame) {
        result.error = frame.error().message;
      } else if (auto json_utf8 = DecodeStreamJsonFrame(*frame)) {
        nlohmann::json root = nlohmann::json::parse(*json_utf8, nullptr, false);
        if (root.is_discarded() || !root.is_object()) {
          result.error = "invalid circuit-relay json";
        } else if (!sessions) {
          result.error = "circuit-relay service not ready";
        } else {
          result = RelayBridge(*sessions, root, stream, policy);
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
    }).detach();
  }
};

CircuitRelayService::CircuitRelayService(Libp2pHost& host, PeerSessionManager& sessions)
    : impl_(std::make_unique<Impl>()), host_(host), sessions_(sessions) {
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
}

void CircuitRelayService::SetAdmissionPolicy(CircuitRelayAdmissionPolicy policy) {
  std::lock_guard<std::mutex> lock(impl_->handler_mutex);
  impl_->admission = std::move(policy);
}

Roe<CircuitRelayBridgeResult> CircuitRelayService::RequestBridge(const std::string& relay_peer_key,
                                                                 const std::string& target_multiaddr,
                                                                 int timeout_ms) {
  if (!host_.IsRunning()) {
    return Error("circuit-relay host not running");
  }
  if (!sessions_.IsDialable(relay_peer_key)) {
    return Error("relay peer endpoint not registered");
  }
  if (target_multiaddr.empty()) {
    return Error("missing target_multiaddr");
  }

  nlohmann::json request = {
      {"v", 1},
      {"op", "bridge"},
      {"target_multiaddr", target_multiaddr},
      {"timeout_ms", timeout_ms > 0 ? timeout_ms : 8000},
  };
  auto frame = EncodeStreamJsonFrame(request.dump());
  if (!frame) {
    return frame.error();
  }

  std::shared_ptr<std::promise<Roe<CircuitRelayBridgeResult>>> result_promise =
      std::make_shared<std::promise<Roe<CircuitRelayBridgeResult>>>();
  auto result_future = result_promise->get_future();

  sessions_.OpenStream(relay_peer_key, {ProtocolName{kCircuitRelayProtocolId}},
                       [frame = *frame, result_promise](libp2p::StreamAndProtocolOrError stream_res) {
                         std::thread([frame, result_promise, stream_res = std::move(stream_res)]() mutable {
                           if (!stream_res) {
                             result_promise->set_value(Error("circuit-relay stream open failed"));
                             return;
                           }
                           auto stream = std::move(stream_res.value().stream);
                           if (!WriteExactFrame(stream, frame)) {
                             result_promise->set_value(Error("Failed to send circuit-relay request"));
                             return;
                           }
                           auto response_frame = ReadExactFrame(stream);
                           if (!response_frame) {
                             result_promise->set_value(Error("Failed to read circuit-relay response"));
                             stream->close([](auto&&) {});
                             return;
                           }
                           auto json_utf8 = DecodeStreamJsonFrame(*response_frame);
                           if (!json_utf8) {
                             result_promise->set_value(json_utf8.error());
                             stream->close([](auto&&) {});
                             return;
                           }
                           nlohmann::json root = nlohmann::json::parse(*json_utf8, nullptr, false);
                           if (root.is_discarded() || !root.is_object()) {
                             result_promise->set_value(Error("invalid circuit-relay response"));
                             stream->close([](auto&&) {});
                             return;
                           }
                           CircuitRelayBridgeResult parsed;
                           parsed.ok = root.value("ok", false);
                           parsed.error = root.value("error", "");
                           if (!parsed.ok) {
                             stream->close([](auto&&) {});
                           }
                           result_promise->set_value(parsed);
                         }).detach();
                       });

  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  if (result_future.wait_for(std::chrono::milliseconds(wait_ms)) != std::future_status::ready) {
    return Error("circuit-relay bridge timed out");
  }
  return result_future.get();
}

} // namespace pbr
