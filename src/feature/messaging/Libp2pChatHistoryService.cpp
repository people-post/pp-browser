#include "feature/messaging/Libp2pChatHistoryService.h"

#include "base/messaging/ChatHistoryResponder.h"
#include "base/messaging/ChatHistoryStreamCodec.h"
#include "base/messaging/MessagingLimits.h"
#include "base/messaging/MessagingJson.h"

#include <libp2p/basic/read.hpp>
#include <libp2p/basic/write.hpp>
#include <libp2p/connection/stream.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/peer/protocol.hpp>

#include <chrono>
#include <future>
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
    return Error("Failed to read chat-history frame header");
  }

  std::vector<uint8_t> frame(header.begin(), header.end());
  uint64_t payload_len = 0;
  for (size_t i = 0; i < 8; ++i) {
    payload_len = (payload_len << 8) | frame[i];
  }
  if (payload_len > kMaxRelayEnvelopeJsonBytes) {
    return Error("Chat-history frame too large");
  }

  Bytes payload(payload_len);
  std::promise<outcome::result<void>> body_promise;
  auto body_future = body_promise.get_future();
  libp2p::read(stream, payload, [&](outcome::result<void> result) { body_promise.set_value(result); });
  if (!body_future.get()) {
    return Error("Failed to read chat-history frame body");
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
    return Error("Failed to write chat-history frame");
  }
  return {};
}

} // namespace

struct Libp2pChatHistoryService::Impl {
  explicit Impl(IThreadStore& store_ref, IdentityStore& identity_ref, IPskSessionStore& psk_store_ref)
      : store(store_ref), identity(identity_ref), psk_store(psk_store_ref) {}

  IThreadStore& store;
  IdentityStore& identity;
  IPskSessionStore& psk_store;

  void HandleStream(libp2p::StreamAndProtocol stream_and_protocol) {
    // Protocol handlers run on the host io thread — hop off before blocking reads/writes
    // (dogfood: chat-history sync during Accept deadlocked both peers' io_context; call-media
    // OpenStream never reached newStream and phone listen sat with unread Recv-Q).
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
      auto request = ChatHistoryRequestFromJson(root);
      if (!request) {
        stream->close([](auto&&) {});
        return;
      }

      auto local_identity = identity.Get();
      if (!local_identity) {
        stream->close([](auto&&) {});
        return;
      }

      auto response =
          ChatHistoryResponder::Serve(store, identity, psk_store, *request, local_identity->relay_user_id);
      if (!response) {
        stream->close([](auto&&) {});
        return;
      }

      const std::string response_json = ChatHistoryResponseToJson(*response).dump();
      auto encoded = ChatHistoryStreamCodec::EncodeFrame(response_json);
      if (!encoded) {
        stream->close([](auto&&) {});
        return;
      }
      (void)WriteExactFrame(stream, *encoded);
      stream->close([](auto&&) {});
    }).detach();
  }
};

Libp2pChatHistoryService::Libp2pChatHistoryService(Libp2pHost& host, PeerSessionManager& sessions, IThreadStore& store,
                                                   IdentityStore& identity, IPskSessionStore& psk_store)
    : impl_(std::make_unique<Impl>(store, identity, psk_store)), host_(host), sessions_(sessions), store_(store),
      identity_(identity), psk_store_(psk_store) {}

Libp2pChatHistoryService::~Libp2pChatHistoryService() {
  Stop();
}

void Libp2pChatHistoryService::Start() {
  if (started_ || !host_.IsRunning()) {
    return;
  }
  started_ = true;
  host_.GetHost().setProtocolHandler({ProtocolName{kChatHistoryProtocolId}},
                                     [impl = impl_.get()](libp2p::StreamAndProtocol stream) {
                                       impl->HandleStream(std::move(stream));
                                     });
}

void Libp2pChatHistoryService::Stop() {
  started_ = false;
}

void Libp2pChatHistoryService::RegisterPeerEndpoint(const std::string& peer_relay_user_id,
                                                    const std::string& multiaddr) {
  (void)sessions_.RegisterEndpoint(peer_relay_user_id, multiaddr);
}

bool Libp2pChatHistoryService::IsPeerReachable(const std::string& peer_identity_value) const {
  return sessions_.IsDialable(peer_identity_value);
}

Roe<ChatHistoryResponse> Libp2pChatHistoryService::FetchChatHistory(const ChatHistoryRequest& request) {
  if (!started_ || !host_.IsRunning()) {
    return Error("libp2p chat-history service not started");
  }
  if (!sessions_.IsDialable(request.peer_identity_value)) {
    return Error("Peer-direct endpoint not registered");
  }

  std::shared_ptr<std::promise<Roe<ChatHistoryResponse>>> result_promise =
      std::make_shared<std::promise<Roe<ChatHistoryResponse>>>();
  auto result_future = result_promise->get_future();

  sessions_.OpenStream(request.peer_identity_value, {ProtocolName{kChatHistoryProtocolId}},
                       [request, result_promise](libp2p::StreamAndProtocolOrError stream_res) {
                         // newStream callbacks run on the host io thread — hop off before blocking I/O.
                         std::thread([request, result_promise, stream_res = std::move(stream_res)]() mutable {
                           auto finish = [&](Roe<ChatHistoryResponse> value) {
                             try {
                               result_promise->set_value(std::move(value));
                             } catch (const std::future_error&) {
                             }
                           };
                           if (!stream_res) {
                             finish(Error("libp2p stream open failed"));
                             return;
                           }
                           auto stream = std::move(stream_res.value().stream);
                           const std::string request_json = ChatHistoryRequestToJson(request).dump();
                           auto frame = ChatHistoryStreamCodec::EncodeFrame(request_json);
                           if (!frame) {
                             finish(frame.error());
                             return;
                           }
                           if (!WriteExactFrame(stream, *frame)) {
                             finish(Error("Failed to send chat-history request"));
                             return;
                           }
                           auto response_frame = ReadExactFrame(stream);
                           stream->close([](auto&&) {});
                           if (!response_frame) {
                             finish(response_frame.error());
                             return;
                           }
                           auto response_json = ChatHistoryStreamCodec::DecodeFrame(*response_frame);
                           if (!response_json) {
                             finish(response_json.error());
                             return;
                           }
                           nlohmann::json root = nlohmann::json::parse(*response_json, nullptr, false);
                           if (root.is_discarded()) {
                             finish(Error("Invalid chat-history response JSON"));
                             return;
                           }
                           finish(ChatHistoryResponseFromJson(root));
                         }).detach();
                       });

  // Must not block forever if the peer's host io is wedged / never answers.
  constexpr int kChatHistoryFetchTimeoutMs = 8000;
  if (result_future.wait_for(std::chrono::milliseconds(kChatHistoryFetchTimeoutMs)) !=
      std::future_status::ready) {
    return Error("libp2p chat-history fetch timed out");
  }
  return result_future.get();
}

} // namespace pbr
