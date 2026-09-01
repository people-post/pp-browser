#include "feature/messaging/AmpChatHistoryService.h"

#include "base/messaging/ChatHistoryResponder.h"
#include "base/messaging/MessagingJson.h"
#include "base/messaging/MessagingLimits.h"
#include "lib/amp/L3/ChannelPolicy.h"
#include "lib/amp/L3/ChannelSession.h"
#include "lib/amp/L3/Types.h"

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <vector>
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

void RunWorker(const AmpChatHistoryService::WorkerPost& post_worker, std::function<void()> task) {
  if (post_worker) {
    post_worker(std::move(task));
  } else {
    task();
  }
}

std::chrono::milliseconds RemainingTimeout(const Clock::time_point deadline) {
  const auto now = Clock::now();
  if (now >= deadline) {
    return std::chrono::milliseconds(1);
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

} // namespace

struct AmpChatHistoryService::Impl {
  Impl(IThreadStore& store_in, IdentityStore& identity_in, IPskSessionStore& psk_store_in)
      : store(store_in), identity(identity_in), psk_store(psk_store_in) {}

  IThreadStore& store;
  IdentityStore& identity;
  IPskSessionStore& psk_store;
  amp::PeerLinkManager* links = nullptr;
  IoPump io_pump;
  WorkerPost post_worker;
  std::atomic<bool> stopped{false};

  void IoPumpUntil(const std::function<bool()>& done, const Clock::time_point deadline) {
    while (!done() && Clock::now() < deadline) {
      if (io_pump) {
        io_pump();
      }
    }
  }

  void HandleInboundChannel(amp::PeerLink& link, const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire)) {
      return;
    }
    auto session = std::make_shared<amp::ChannelSession>();
    session->Bind(*link.Mux(), channel_id, amp::ControlJsonChannelPolicy(),
                  [this, session](Roe<std::vector<uint8_t>> frame) {
                    if (!frame || stopped.load(std::memory_order_acquire)) {
                      return false;
                    }
                    auto body = std::move(*frame);
                    RunWorker(post_worker, [this, session, body = std::move(body)]() mutable {
                      if (stopped.load(std::memory_order_acquire)) {
                        return;
                      }
                      const std::string json_utf8(body.begin(), body.end());
                      auto root = TryParseObject(json_utf8);
                      if (!root) {
                        return;
                      }
                      auto request = ChatHistoryRequestFromJson(*root);
                      if (!request) {
                        return;
                      }
                      auto local_identity = identity.Get();
                      if (!local_identity) {
                        return;
                      }
                      auto response = ChatHistoryResponder::Serve(store, identity, psk_store, *request,
                                                                  local_identity->relay_user_id);
                      if (!response) {
                        return;
                      }
                      const std::string response_json = DumpJson(ChatHistoryResponseToJson(*response));
                      if (!session->EnqueueOutbound(JsonToBody(response_json))) {
                        return;
                      }
                      if (io_pump) {
                        io_pump();
                      }
                    });
                    return false;
                  });
  }
};

AmpChatHistoryService::AmpChatHistoryService(amp::PeerLinkManager& links, IoPump io_pump, IThreadStore& store,
                                             IdentityStore& identity, IPskSessionStore& psk_store,
                                             WorkerPost post_worker)
    : impl_(std::make_unique<Impl>(store, identity, psk_store)), links_(links), io_pump_(std::move(io_pump)),
      post_worker_(std::move(post_worker)) {
  impl_->links = &links_;
  impl_->io_pump = io_pump_;
  impl_->post_worker = post_worker_;
}

AmpChatHistoryService::~AmpChatHistoryService() {
  Stop();
}

void AmpChatHistoryService::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  impl_->stopped.store(false, std::memory_order_release);
  links_.SetProtocolHandler(kChatHistoryProtocolId, [impl = impl_.get()](amp::PeerLink& link, const uint32_t channel_id) {
    impl->HandleInboundChannel(link, channel_id);
  });
}

void AmpChatHistoryService::Stop() {
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  links_.RemoveProtocolHandler(kChatHistoryProtocolId);
}

void AmpChatHistoryService::RegisterPeerEndpoint(const std::string& peer_relay_user_id, const std::string& multiaddr) {
  (void)links_.RegisterEndpoint(peer_relay_user_id, multiaddr);
}

bool AmpChatHistoryService::IsPeerReachable(const std::string& peer_identity_value) const {
  return links_.GetLinkSnapshot(peer_identity_value).has_endpoint;
}

Roe<ChatHistoryResponse> AmpChatHistoryService::FetchChatHistory(const ChatHistoryRequest& request) {
  if (!started_) {
    return Error("amp chat-history service not started");
  }
  if (!IsPeerReachable(request.peer_identity_value)) {
    return Error("Peer-direct endpoint not registered");
  }

  constexpr auto kFetchTimeout = std::chrono::milliseconds(8000);
  const auto deadline = Clock::now() + kFetchTimeout;

  auto result_promise = std::make_shared<std::promise<Roe<std::string>>>();
  auto result_future = result_promise->get_future();
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto session = std::make_shared<amp::ChannelSession>();

  auto finish = [settled, result_promise, session](Roe<std::string> value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    session->Close();
    try {
      result_promise->set_value(std::move(value));
    } catch (const std::future_error&) {
    }
  };

  const std::string peer_key = request.peer_identity_value;
  const std::string request_json = DumpJson(ChatHistoryRequestToJson(request));
  const auto read_timeout = RemainingTimeout(deadline);

  links_.EnsureAssociation(peer_key, [this, peer_key, request_json, finish, settled, session, deadline,
                                      read_timeout](amp::PeerLinkManager::LinkRoe assoc) mutable {
    if (!assoc) {
      finish(Error(assoc.error().message));
      return;
    }
    links_.OpenChannel(peer_key, kChatHistoryProtocolId, amp::ControlJsonChannelPolicy(read_timeout),
                       [this, peer_key, request_json, finish, settled, session, deadline,
                        read_timeout](amp::PeerLinkManager::ChannelRoe channel) mutable {
                         if (!channel) {
                           finish(Error(channel.error().message));
                           return;
                         }
                         impl_->IoPumpUntil(
                             [&] {
                               auto* link = links_.FindLink(peer_key);
                               return link && link->Mux() &&
                                      link->Mux()->State(*channel) == amp::ChannelState::Open;
                             },
                             deadline);
                         auto* link = links_.FindLink(peer_key);
                         if (!link || !link->Mux() || link->Mux()->State(*channel) != amp::ChannelState::Open) {
                           finish(Error("amp chat-history: channel open failed"));
                           return;
                         }

                         session->Bind(*link->Mux(), *channel, amp::ControlJsonChannelPolicy(read_timeout),
                                       [finish](Roe<std::vector<uint8_t>> frame) {
                                         if (!frame) {
                                           finish(Error("Failed to read chat-history response"));
                                           return false;
                                         }
                                         finish(std::string(frame->begin(), frame->end()));
                                         return false;
                                       });

                         if (!session->EnqueueOutbound(JsonToBody(request_json))) {
                           finish(Error("Failed to send chat-history request"));
                           return;
                         }

                         impl_->IoPumpUntil([settled] { return settled->load(std::memory_order_acquire); }, deadline);
                         if (!settled->load(std::memory_order_acquire)) {
                           finish(Error("amp chat-history fetch timed out"));
                         }
                       });
  });

  impl_->IoPumpUntil([&] { return result_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; },
                     deadline);

  if (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    finish(Error("amp chat-history fetch timed out"));
    return Error("amp chat-history fetch timed out");
  }
  auto response_json = result_future.get();
  if (!response_json) {
    return response_json.error();
  }
  auto root = TryParseObject(*response_json);
  if (!root) {
    return Error("Invalid chat-history response JSON");
  }
  return ChatHistoryResponseFromJson(*root);
}

} // namespace pbr
