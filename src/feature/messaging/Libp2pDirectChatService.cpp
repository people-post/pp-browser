#include "feature/messaging/Libp2pDirectChatService.h"

#include "base/messaging/ChatHistoryStreamCodec.h"
#include "base/messaging/MessagingJson.h"
#include "base/messaging/MessagingLimits.h"

#include <libp2p/basic/read.hpp>
#include <libp2p/basic/write.hpp>
#include <libp2p/connection/stream.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/peer/protocol.hpp>

#include <chrono>
#include <future>
#include <mutex>
#include <nlohmann/json.hpp>

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
    return Error("Failed to read chat frame header");
  }

  std::vector<uint8_t> frame(header.begin(), header.end());
  uint64_t payload_len = 0;
  for (size_t i = 0; i < 8; ++i) {
    payload_len = (payload_len << 8) | frame[i];
  }
  if (payload_len > kMaxRelayEnvelopeJsonBytes) {
    return Error("Chat frame too large");
  }

  Bytes payload(payload_len);
  std::promise<outcome::result<void>> body_promise;
  auto body_future = body_promise.get_future();
  libp2p::read(stream, payload, [&](outcome::result<void> result) { body_promise.set_value(result); });
  if (!body_future.get()) {
    return Error("Failed to read chat frame body");
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
    return Error("Failed to write chat frame");
  }
  return {};
}

} // namespace

struct Libp2pDirectChatService::Impl {
  std::mutex handler_mutex;
  InboundHandler inbound;

  void HandleStream(libp2p::StreamAndProtocol stream_and_protocol) {
    // Protocol handlers run on the host io thread — hop off before blocking
    // reads and before ApplyInboundControl (call media / mic open must not
    // stall libp2p for both peers).
    auto stream = std::move(stream_and_protocol.stream);
    std::thread([this, stream = std::move(stream)]() mutable {
      auto frame = ReadExactFrame(stream);
      if (!frame) {
        stream->close([](auto&&) {});
        return;
      }
      auto json_utf8 = ChatHistoryStreamCodec::DecodeFrame(*frame);
      if (!json_utf8) {
        stream->close([](auto&&) {});
        return;
      }
      nlohmann::json root = nlohmann::json::parse(*json_utf8, nullptr, false);
      if (root.is_discarded()) {
        stream->close([](auto&&) {});
        return;
      }
      auto envelope = ParseRelayEnvelope(root);
      if (!envelope) {
        stream->close([](auto&&) {});
        return;
      }

      // Tiny ack so sender can treat write+read as success.
      static const std::string kAck = R"({"ok":true})";
      if (auto encoded = ChatHistoryStreamCodec::EncodeFrame(kAck)) {
        (void)WriteExactFrame(stream, *encoded);
      }
      stream->close([](auto&&) {});

      InboundHandler handler;
      {
        std::lock_guard lock(handler_mutex);
        handler = inbound;
      }
      if (handler) {
        handler(std::move(*envelope));
      }
    }).detach();
  }
};

Libp2pDirectChatService::Libp2pDirectChatService(Libp2pHost& host, PeerSessionManager& sessions)
    : impl_(std::make_unique<Impl>()), host_(host), sessions_(sessions) {}

Libp2pDirectChatService::~Libp2pDirectChatService() {
  Stop();
}

void Libp2pDirectChatService::Start() {
  if (started_ || !host_.IsRunning()) {
    return;
  }
  started_ = true;
  host_.GetHost().setProtocolHandler({ProtocolName{kDirectChatProtocolId}},
                                     [impl = impl_.get()](libp2p::StreamAndProtocol stream) {
                                       impl->HandleStream(std::move(stream));
                                     });
}

void Libp2pDirectChatService::Stop() {
  started_ = false;
  std::lock_guard lock(impl_->handler_mutex);
  impl_->inbound = nullptr;
}

void Libp2pDirectChatService::SetInboundHandler(InboundHandler handler) {
  std::lock_guard lock(impl_->handler_mutex);
  impl_->inbound = std::move(handler);
}

bool Libp2pDirectChatService::IsPeerReachable(const std::string& peer_identity_value) const {
  return sessions_.IsDialable(peer_identity_value);
}

Roe<void> Libp2pDirectChatService::SendEnvelope(const std::string& peer_relay_user_id,
                                                const RelayEnvelope& envelope) {
  if (!started_ || !host_.IsRunning()) {
    return Error("libp2p direct chat service not started");
  }
  if (!sessions_.IsDialable(peer_relay_user_id)) {
    return Error("Peer-direct endpoint not registered")
        .WithUser("No usable peer address — add a dialable multiaddr on the contact.");
  }

  const std::string envelope_json = RelayEnvelopeToJson(envelope).dump();
  auto frame = ChatHistoryStreamCodec::EncodeFrame(envelope_json);
  if (!frame) {
    return frame.error();
  }

  std::shared_ptr<std::promise<Roe<void>>> result_promise = std::make_shared<std::promise<Roe<void>>>();
  auto result_future = result_promise->get_future();

  sessions_.OpenStream(peer_relay_user_id, {ProtocolName{kDirectChatProtocolId}},
                       [frame = *frame, result_promise](libp2p::StreamAndProtocolOrError stream_res) {
                         // newStream callbacks run on the host io thread — hop off before blocking I/O.
                         std::thread([frame, result_promise, stream_res = std::move(stream_res)]() mutable {
                           if (!stream_res) {
                             try {
                               result_promise->set_value(
                                   Error("libp2p chat stream open failed")
                                       .WithUser("Reached the peer but chat handshake failed."));
                             } catch (const std::future_error&) {
                             }
                             return;
                           }
                           auto stream = std::move(stream_res.value().stream);
                           if (!WriteExactFrame(stream, frame)) {
                             try {
                               result_promise->set_value(
                                   Error("Failed to send direct chat envelope")
                                       .WithUser("Direct send didn't confirm — will use relay if available."));
                             } catch (const std::future_error&) {
                             }
                             return;
                           }
                           auto ack_frame = ReadExactFrame(stream);
                           stream->close([](auto&&) {});
                           if (!ack_frame) {
                             try {
                               result_promise->set_value(
                                   Error("Failed to read direct chat ack")
                                       .WithUser("Direct send didn't confirm — will use relay if available."));
                             } catch (const std::future_error&) {
                             }
                             return;
                           }
                           try {
                             result_promise->set_value({});
                           } catch (const std::future_error&) {
                           }
                         }).detach();
                       });

  // Must not block Browser IO forever — call MediaKey/roster used to stall Accept/Connect
  // when mDNS marked the peer dialable but chat open/ack never completed.
  constexpr int kDirectChatSendTimeoutMs = 4000;
  if (result_future.wait_for(std::chrono::milliseconds(kDirectChatSendTimeoutMs)) !=
      std::future_status::ready) {
    return Error("libp2p chat send timed out")
        .WithUser("Direct send didn't confirm — will use relay if available.");
  }
  return result_future.get();
}

} // namespace pbr
