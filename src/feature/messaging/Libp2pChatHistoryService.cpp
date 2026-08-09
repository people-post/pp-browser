#include "feature/messaging/Libp2pChatHistoryService.h"

#include "base/messaging/ChatHistoryResponder.h"
#include "base/messaging/MessagingLimits.h"
#include "base/messaging/MessagingJson.h"
#include "libp2p/integration/host/Libp2pWorker.h"
#include "libp2p/integration/host/StreamFrameIo.h"
#include "libp2p/integration/host/StreamJsonFrame.h"

#include <libp2p/connection/stream.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/peer/protocol.hpp>

#include <chrono>
#include <future>
#include <mutex>
#include <nlohmann/json.hpp>

namespace pbr {

namespace {

using libp2p::connection::Stream;
using libp2p::peer::ProtocolName;
using Clock = std::chrono::steady_clock;

void CloseQuiet(const std::shared_ptr<Stream>& stream) {
  if (stream) {
    stream->close([](auto&&) {});
  }
}

std::chrono::milliseconds RemainingTimeout(const Clock::time_point& deadline) {
  const auto now = Clock::now();
  if (now >= deadline) {
    return std::chrono::milliseconds(1);
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

} // namespace

struct Libp2pChatHistoryService::Impl {
  explicit Impl(IThreadStore& store_ref, IdentityStore& identity_ref, IPskSessionStore& psk_store_ref)
      : store(store_ref), identity(identity_ref), psk_store(psk_store_ref) {}

  IThreadStore& store;
  IdentityStore& identity;
  IPskSessionStore& psk_store;
  Libp2pHost* host = nullptr;

  void HandleStream(libp2p::StreamAndProtocol stream_and_protocol) {
    if (!host) {
      return;
    }
    auto stream = std::move(stream_and_protocol.stream);
    PostLibp2pWorker(*host, WorkerLane::Normal, [this, stream = std::move(stream)]() mutable {
      auto json_utf8 = BlockingReadStreamJson(stream, kMaxRelayEnvelopeJsonBytes,
                                              kDefaultControlFrameReadTimeout);
      if (!json_utf8) {
        CloseQuiet(stream);
        return;
      }

      nlohmann::json root = nlohmann::json::parse(*json_utf8, nullptr, false);
      if (root.is_discarded()) {
        CloseQuiet(stream);
        return;
      }
      auto request = ChatHistoryRequestFromJson(root);
      if (!request) {
        CloseQuiet(stream);
        return;
      }

      auto local_identity = identity.Get();
      if (!local_identity) {
        CloseQuiet(stream);
        return;
      }

      auto response =
          ChatHistoryResponder::Serve(store, identity, psk_store, *request, local_identity->relay_user_id);
      if (!response) {
        CloseQuiet(stream);
        return;
      }

      const std::string response_json = ChatHistoryResponseToJson(*response).dump();
      (void)BlockingWriteStreamJson(stream, response_json, kMaxRelayEnvelopeJsonBytes);
      CloseQuiet(stream);
    });
  }
};

Libp2pChatHistoryService::Libp2pChatHistoryService(Libp2pHost& host, PeerSessionManager& sessions, IThreadStore& store,
                                                   IdentityStore& identity, IPskSessionStore& psk_store)
    : impl_(std::make_unique<Impl>(store, identity, psk_store)), host_(host), sessions_(sessions), store_(store),
      identity_(identity), psk_store_(psk_store) {
  impl_->host = &host_;
}

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

  constexpr auto kChatHistoryFetchTimeout = std::chrono::milliseconds(8000);
  const auto deadline = Clock::now() + kChatHistoryFetchTimeout;

  // OpenStream callback only delivers the stream; blocking read/write stays on THIS worker.
  using StreamOpenResult = libp2p::StreamAndProtocolOrError;
  auto open_promise = std::make_shared<std::promise<StreamOpenResult>>();
  auto open_future = open_promise->get_future();
  auto active_mu = std::make_shared<std::mutex>();
  auto active_stream = std::make_shared<std::shared_ptr<Stream>>();

  sessions_.OpenStream(request.peer_identity_value, {ProtocolName{kChatHistoryProtocolId}},
                       [open_promise, active_mu, active_stream](StreamOpenResult stream_res) {
                         if (stream_res) {
                           std::lock_guard lock(*active_mu);
                           *active_stream = stream_res.value().stream;
                         }
                         try {
                           open_promise->set_value(std::move(stream_res));
                         } catch (const std::future_error&) {
                         }
                       });

  if (open_future.wait_until(deadline) != std::future_status::ready) {
    std::shared_ptr<Stream> to_reset;
    {
      std::lock_guard lock(*active_mu);
      to_reset = *active_stream;
      active_stream->reset();
    }
    ResetStreamQuiet(to_reset);
    return Error("libp2p chat-history fetch timed out");
  }
  auto stream_res = open_future.get();
  if (!stream_res) {
    return Error("libp2p stream open failed");
  }
  auto stream = std::move(stream_res.value().stream);
  {
    std::lock_guard lock(*active_mu);
    *active_stream = stream;
  }

  const std::string request_json = ChatHistoryRequestToJson(request).dump();
  if (!BlockingWriteStreamJson(stream, request_json, kMaxRelayEnvelopeJsonBytes)) {
    CloseQuiet(stream);
    return Error("Failed to send chat-history request");
  }
  auto response_json =
      BlockingReadStreamJson(stream, kMaxRelayEnvelopeJsonBytes, RemainingTimeout(deadline));
  {
    std::lock_guard lock(*active_mu);
    active_stream->reset();
  }
  CloseQuiet(stream);
  if (!response_json) {
    return response_json.error();
  }
  nlohmann::json root = nlohmann::json::parse(*response_json, nullptr, false);
  if (root.is_discarded()) {
    return Error("Invalid chat-history response JSON");
  }
  return ChatHistoryResponseFromJson(root);
}

} // namespace pbr
