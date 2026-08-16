#include "feature/messaging/Libp2pChatHistoryService.h"

#include "base/messaging/ChatHistoryResponder.h"
#include "base/messaging/MessagingLimits.h"
#include "base/messaging/MessagingJson.h"
#include "libp2p/integration/host/Libp2pWorker.h"
#include "libp2p/integration/host/StreamFrameIo.h"

#include <libp2p/connection/stream.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/peer/protocol.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

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

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
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
  std::atomic<bool> stopped{false};

  void HandleStream(libp2p::StreamAndProtocol stream_and_protocol) {
    if (!host || stopped.load(std::memory_order_acquire)) {
      return;
    }
    auto stream = std::move(stream_and_protocol.stream);
    if (!stream) {
      return;
    }
    auto duplex = std::make_shared<DuplexFrameSession>();
    auto policy = ControlJsonIoPolicy(host->IoExecutor(), kDefaultControlFrameReadTimeout,
                                      kMaxRelayEnvelopeJsonBytes);
    duplex->Start(
        stream,
        [this, duplex, stream](Roe<std::vector<uint8_t>> frame) {
          if (!frame || stopped.load(std::memory_order_acquire)) {
            return false;
          }
          auto body = *frame;
          PostLibp2pWorker(*host, WorkerLane::Normal, [this, duplex, stream, body = std::move(body)]() mutable {
            auto fail = [host = host, duplex, stream]() {
              host->Post([duplex, stream]() {
                duplex->Stop();
                CloseQuiet(stream);
              });
            };
            if (stopped.load(std::memory_order_acquire)) {
              fail();
              return;
            }
            const std::string json_utf8(body.begin(), body.end());
            nlohmann::json root = nlohmann::json::parse(json_utf8, nullptr, false);
            if (root.is_discarded()) {
              fail();
              return;
            }
            auto request = ChatHistoryRequestFromJson(root);
            if (!request) {
              fail();
              return;
            }
            auto local_identity = identity.Get();
            if (!local_identity) {
              fail();
              return;
            }
            auto response =
                ChatHistoryResponder::Serve(store, identity, psk_store, *request, local_identity->relay_user_id);
            if (!response) {
              fail();
              return;
            }
            const std::string response_json = ChatHistoryResponseToJson(*response).dump();
            auto payload = JsonToBody(response_json);
            host->Post([duplex, stream, payload = std::move(payload)]() mutable {
              if (!duplex->EnqueueOutbound(std::move(payload), [duplex, stream](Roe<void>) {
                    duplex->Stop();
                    CloseQuiet(stream);
                  })) {
                duplex->Stop();
                CloseQuiet(stream);
              }
            });
          });
          return true;
        },
        [this]() { return stopped.load(std::memory_order_acquire); }, std::move(policy));
  }
};

Libp2pChatHistoryService::Libp2pChatHistoryService(Libp2pHost& host, PeerSessionManager& sessions, IThreadStore& store,
                                                   IdentityStore& identity, IPskSessionStore& psk_store)
    : impl_(std::make_unique<Impl>(store, identity, psk_store)), host_(host), sessions_(sessions) {
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
  impl_->stopped.store(false, std::memory_order_release);
  host_.GetHost().setProtocolHandler({ProtocolName{kChatHistoryProtocolId}},
                                     [impl = impl_.get()](libp2p::StreamAndProtocol stream) {
                                       impl->HandleStream(std::move(stream));
                                     });
}

void Libp2pChatHistoryService::Stop() {
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
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

  auto result_promise = std::make_shared<std::promise<Roe<std::string>>>();
  auto result_future = result_promise->get_future();
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto active_mu = std::make_shared<std::mutex>();
  auto active_stream = std::make_shared<std::shared_ptr<Stream>>();

  auto finish = [settled, result_promise, active_mu, active_stream](Roe<std::string> value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    {
      std::lock_guard lock(*active_mu);
      active_stream->reset();
    }
    try {
      result_promise->set_value(std::move(value));
    } catch (const std::future_error&) {
    }
  };

  const std::string request_json = ChatHistoryRequestToJson(request).dump();
  sessions_.OpenStream(request.peer_identity_value, {ProtocolName{kChatHistoryProtocolId}},
                       [host = &host_, request_json, deadline, finish, settled, active_mu,
                        active_stream](libp2p::StreamAndProtocolOrError stream_res) mutable {
                         if (!stream_res) {
                           finish(Error("libp2p stream open failed"));
                           return;
                         }
                         auto stream = std::move(stream_res.value().stream);
                         {
                           std::lock_guard lock(*active_mu);
                           *active_stream = stream;
                         }
                         host->Post([host, stream = std::move(stream), request_json, deadline, finish, settled,
                                     active_mu, active_stream]() mutable {
                           if (settled->load(std::memory_order_acquire)) {
                             CloseQuiet(stream);
                             return;
                           }
                           auto duplex = std::make_shared<DuplexFrameSession>();
                           auto policy = ControlJsonIoPolicy(host->IoExecutor(), RemainingTimeout(deadline),
                                                             kMaxRelayEnvelopeJsonBytes);
                           if (!duplex->EnqueueOutbound(JsonToBody(request_json))) {
                             finish(Error("Failed to send chat-history request"));
                             return;
                           }
                           duplex->Start(
                               stream,
                               [finish, duplex](Roe<std::vector<uint8_t>> frame) {
                                 if (!frame) {
                                   finish(Error("Failed to read chat-history response"));
                                   return false;
                                 }
                                 finish(std::string(frame->begin(), frame->end()));
                                 return false;
                               },
                               [] { return false; }, std::move(policy),
                               [finish, duplex](const char* reason) {
                                 const char* tag = (reason && reason[0]) ? reason : "unknown";
                                 if (std::string(tag) == "handler_close") {
                                   return;
                                 }
                                 finish(Error("Failed to read chat-history response"));
                               });
                         });
                       });

  if (result_future.wait_until(deadline) != std::future_status::ready) {
    std::shared_ptr<Stream> to_reset;
    {
      std::lock_guard lock(*active_mu);
      to_reset = *active_stream;
      active_stream->reset();
    }
    ResetStreamQuiet(to_reset);
    finish(Error("libp2p chat-history fetch timed out"));
    return Error("libp2p chat-history fetch timed out");
  }
  auto response_json = result_future.get();
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
