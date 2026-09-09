#include "feature/conversations/AmpChatHistoryTransport.h"

#include "amp/link/PeerLink.h"

#include "domain/messaging/ChatHistoryResponder.h"
#include "common/chat/MessagingJson.h"
#include "common/chat/MessagingLimits.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/L3/Types.h"

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <vector>
#include "common/ValueJson.h"
#include "common/PbrCompat.h"
#include "domain/mesh/shared/AmpParkUntil.h"

namespace pbr {

namespace {

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

void RunWorker(const AmpChatHistoryTransport::WorkerPost& post_worker, std::function<void()> task) {
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

struct AmpChatHistoryTransport::Impl {
  Impl(IThreadStore& store_in, IdentityStore& identity_in, IPskSessionStore& psk_store_in)
      : store(store_in), identity(identity_in), psk_store(psk_store_in) {}

  IThreadStore& store;
  IdentityStore& identity;
  IPskSessionStore& psk_store;
  IChatPeerLinks* links = nullptr;
  IoPump io_pump;
  WorkerPost post_worker;
  IoPost post_io;
  std::atomic<bool> stopped{false};


  void ServeRequest(std::shared_ptr<pp::amp::ChannelSession> session, std::vector<uint8_t> body) {
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
                      const std::string local_account_id = !local_identity->account_id.empty()
                                                               ? local_identity->account_id
                                                               : local_identity->relay_user_id;
                      auto response = ChatHistoryResponder::Serve(
                          store, psk_store, *request, local_identity->relay_user_id, local_account_id,
                          [this](const std::vector<uint8_t>& bytes) { return identity.SignBytes(bytes); });
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
  }

  void HandleInboundChannel(pp::amp::PeerLink& link, const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire)) {
      return;
    }
    auto session = std::make_shared<pp::amp::ChannelSession>();
    session->Bind(*link.Mux(), channel_id, pp::amp::ControlJsonChannelPolicy(),
                  [this, session](Roe<std::vector<uint8_t>> frame) {
                    if (!frame || stopped.load(std::memory_order_acquire)) {
                      return false;
                    }
                    ServeRequest(session, std::move(*frame));
                    return false;
                  });
  }
};

AmpChatHistoryTransport::AmpChatHistoryTransport(IChatPeerLinks& links, IoPump io_pump, IThreadStore& store,
                                             IdentityStore& identity, IPskSessionStore& psk_store,
                                             WorkerPost post_worker, IoPost post_io)
    : impl_(std::make_unique<Impl>(store, identity, psk_store)), links_(links), io_pump_(std::move(io_pump)),
      post_worker_(std::move(post_worker)), post_io_(std::move(post_io)) {
  impl_->links = &links_;
  impl_->io_pump = io_pump_;
  impl_->post_worker = post_worker_;
  impl_->post_io = post_io_;
}

AmpChatHistoryTransport::~AmpChatHistoryTransport() {
  Stop();
}

void AmpChatHistoryTransport::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  impl_->stopped.store(false, std::memory_order_release);
  links_.SetProtocolHandler(kChatHistoryProtocolId, [impl = impl_.get()](pp::amp::PeerLink& link, const uint32_t channel_id) {
    impl->HandleInboundChannel(link, channel_id);
  });
}

void AmpChatHistoryTransport::Stop() {
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  links_.RemoveProtocolHandler(kChatHistoryProtocolId);
}

void AmpChatHistoryTransport::RegisterPeerEndpoint(const std::string& peer_relay_user_id, const std::string& multiaddr) {
  (void)links_.RegisterEndpoint(peer_relay_user_id, multiaddr);
}

bool AmpChatHistoryTransport::IsPeerReachable(const std::string& peer_identity_value) const {
  return links_.GetLinkSnapshot(peer_identity_value).has_endpoint;
}

void AmpChatHistoryTransport::FetchChatHistoryAsync(const ChatHistoryRequest& request,
                                                    std::function<void(Roe<ChatHistoryResponse>)> on_done) {
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto finish_once = std::make_shared<std::function<void(Roe<ChatHistoryResponse>)>>();
  *finish_once = [on_done = std::move(on_done), settled](Roe<ChatHistoryResponse> value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    if (on_done) {
      on_done(std::move(value));
    }
  };

  if (!started_) {
    (*finish_once)(Error("amp chat-history service not started"));
    return;
  }
  if (!IsPeerReachable(request.peer_identity_value)) {
    (*finish_once)(Error("Peer-direct endpoint not registered"));
    return;
  }

  constexpr auto kFetchTimeout = std::chrono::milliseconds(8000);
  const auto deadline = Clock::now() + kFetchTimeout;
  auto session = std::make_shared<pp::amp::ChannelSession>();
  auto finish = std::make_shared<std::function<void(Roe<std::string>)>>();
  *finish = [finish_once, session](Roe<std::string> value) {
    session->Close();
    if (!value) {
      (*finish_once)(value.error());
      return;
    }
    auto root = TryParseObject(*value);
    if (!root) {
      (*finish_once)(Error("Invalid chat-history response JSON"));
      return;
    }
    (*finish_once)(ChatHistoryResponseFromJson(*root));
  };

  const std::string peer_key = request.peer_identity_value;
  const std::string request_json = DumpJson(ChatHistoryRequestToJson(request));
  const auto read_timeout = RemainingTimeout(deadline);

  links_.EnsureAssociation(peer_key, [this, peer_key, request_json, finish, settled, session, deadline,
                                      read_timeout](IChatPeerLinks::LinkRoe assoc) mutable {
    if (!assoc) {
      (*finish)(Error(assoc.error().message));
      return;
    }
    links_.OpenChannel(peer_key, kChatHistoryProtocolId, pp::amp::ControlJsonChannelPolicy(read_timeout),
                       [this, peer_key, request_json, finish, settled, session, deadline,
                        read_timeout](IChatPeerLinks::ChannelRoe channel) mutable {
                         if (!channel) {
                           (*finish)(Error(channel.error().message));
                           return;
                         }
                         AmpScheduleWhenChannelOpen(
                             post_io_, io_pump_,
                             [this, peer_key, channel_id = *channel]() {
                               auto* link = links_.FindLink(peer_key);
                               return link && link->Mux() &&
                                      link->Mux()->State(channel_id) == pp::amp::ChannelState::Open;
                             },
                             deadline,
                             [this, peer_key, channel_id = *channel, request_json, finish, settled, session, deadline,
                              read_timeout](bool open) mutable {
                               if (!open) {
                                 (*finish)(Error("amp chat-history: channel open failed"));
                                 return;
                               }
                               auto* link = links_.FindLink(peer_key);
                               if (!link || !link->Mux() ||
                                   link->Mux()->State(channel_id) != pp::amp::ChannelState::Open) {
                                 (*finish)(Error("amp chat-history: channel open failed"));
                                 return;
                               }

                               session->Bind(*link->Mux(), channel_id, pp::amp::ControlJsonChannelPolicy(read_timeout),
                                             [finish](Roe<std::vector<uint8_t>> frame) {
                                               if (!frame) {
                                                 (*finish)(Error("Failed to read chat-history response"));
                                                 return false;
                                               }
                                               (*finish)(std::string(frame->begin(), frame->end()));
                                               return false;
                                             });

                               if (!session->EnqueueOutbound(JsonToBody(request_json))) {
                                 (*finish)(Error("Failed to send chat-history request"));
                                 return;
                               }

                               AmpScheduleUntilSettled(post_io_, io_pump_, settled, deadline, [finish]() {
                                 (*finish)(Error("amp chat-history fetch timed out"));
                               });
                             },
                             [this]() { return impl_->stopped.load(std::memory_order_acquire); });
                       });
  });
}

Roe<ChatHistoryResponse> AmpChatHistoryTransport::FetchChatHistory(const ChatHistoryRequest& request) {
  auto result_promise = std::make_shared<std::promise<Roe<ChatHistoryResponse>>>();
  auto result_future = result_promise->get_future();
  constexpr auto kFetchTimeout = std::chrono::milliseconds(8000);
  const auto deadline = Clock::now() + kFetchTimeout;

  FetchChatHistoryAsync(request, [result_promise](Roe<ChatHistoryResponse> value) {
    try {
      result_promise->set_value(std::move(value));
    } catch (const std::future_error&) {
    }
  });

  AmpParkUntil([&] { return result_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; },
               deadline, io_pump_);
  if (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    return Error("amp chat-history fetch timed out");
  }
  return result_future.get();
}

} // namespace pbr
