#include "feature/conversations/AmpDirectChatTransport.h"

#include "amp/link/PeerLink.h"

#include "common/chat/MessagingJson.h"
#include "common/chat/MessagingLimits.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/L3/Types.h"
#include "domain/mesh/shared/AmpParkUntil.h"

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
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

void RunWorker(const AmpDirectChatTransport::WorkerPost& post_worker, std::function<void()> task) {
  if (post_worker) {
    post_worker(std::move(task));
  } else {
    task();
  }
}

} // namespace

struct AmpDirectChatTransport::Impl {
  IChatPeerLinks* links = nullptr;
  IoPump io_pump;
  WorkerPost post_worker;
  IoPost post_io;
  std::mutex handler_mutex;
  InboundHandler inbound;
  std::atomic<bool> stopped{false};

  void HandleInboundChannel(pp::amp::PeerLink& link, const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || !links) {
      return;
    }
    auto session = std::make_shared<pp::amp::ChannelSession>();
    auto policy = pp::amp::ControlJsonChannelPolicy();
    session->Bind(*link.Mux(), channel_id, policy, [this, session](Roe<std::vector<uint8_t>> frame) {
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
        auto envelope = ParseRelayEnvelope(*root);
        if (!envelope) {
          return;
        }

        InboundHandler handler;
        {
          std::lock_guard lock(handler_mutex);
          handler = inbound;
        }
        static const std::string kAck = R"({"ok":true})";
        if (!session->EnqueueOutbound(JsonToBody(kAck))) {
          return;
        }
        if (io_pump) {
          io_pump();
        }
        if (handler) {
          handler(std::move(*envelope));
        }
      });
      return false;
    });
  }
};

AmpDirectChatTransport::AmpDirectChatTransport(IChatPeerLinks& links, IoPump io_pump, WorkerPost post_worker,
                                               IoPost post_io)
    : impl_(std::make_unique<Impl>()), links_(links), io_pump_(std::move(io_pump)),
      post_worker_(std::move(post_worker)), post_io_(std::move(post_io)) {
  impl_->links = &links_;
  impl_->io_pump = io_pump_;
  impl_->post_worker = post_worker_;
  impl_->post_io = post_io_;
}

AmpDirectChatTransport::~AmpDirectChatTransport() {
  Stop();
}

void AmpDirectChatTransport::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  impl_->stopped.store(false, std::memory_order_release);
  links_.SetProtocolHandler(kDirectChatProtocolId, [impl = impl_.get()](pp::amp::PeerLink& link, const uint32_t channel_id) {
    impl->HandleInboundChannel(link, channel_id);
  });
}

void AmpDirectChatTransport::Stop() {
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  links_.RemoveProtocolHandler(kDirectChatProtocolId);
  std::lock_guard lock(impl_->handler_mutex);
  impl_->inbound = nullptr;
}

void AmpDirectChatTransport::SetInboundHandler(InboundHandler handler) {
  std::lock_guard lock(impl_->handler_mutex);
  impl_->inbound = std::move(handler);
}

bool AmpDirectChatTransport::IsPeerReachable(const std::string& peer_identity_value) const {
  return links_.GetLinkSnapshot(peer_identity_value).has_endpoint ||
         links_.IsConnected(peer_identity_value);
}

void AmpDirectChatTransport::SendEnvelopeAsync(const std::string& peer_relay_user_id,
                                               const RelayEnvelope& envelope,
                                               std::function<void(Roe<void>)> on_done) {
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto finish_once = std::make_shared<std::function<void(Roe<void>)>>();
  *finish_once = [on_done = std::move(on_done), settled](Roe<void> value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    if (on_done) {
      on_done(std::move(value));
    }
  };

  if (!started_) {
    (*finish_once)(Error("amp direct chat service not started"));
    return;
  }
  if (!IsPeerReachable(peer_relay_user_id)) {
    (*finish_once)(Error("Peer-direct endpoint not registered")
                       .WithUser("No usable peer address — add a dialable multiaddr on the contact."));
    return;
  }

  const std::string envelope_json = DumpJson(RelayEnvelopeToJson(envelope));
  constexpr auto kSendTimeout = std::chrono::milliseconds(4000);
  const auto deadline = Clock::now() + kSendTimeout;
  auto session = std::make_shared<pp::amp::ChannelSession>();
  auto finish = std::make_shared<std::function<void(Roe<void>)>>();
  *finish = [finish_once, session](Roe<void> value) {
    session->Close();
    (*finish_once)(std::move(value));
  };

  const std::string peer_key = peer_relay_user_id;
  links_.OpenChannel(peer_key, kDirectChatProtocolId, pp::amp::ControlJsonChannelPolicy(),
                     [this, peer_key, envelope_json, finish, settled, session,
                      deadline](IChatPeerLinks::ChannelRoe channel) mutable {
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
                           [this, peer_key, channel_id = *channel, envelope_json, finish, settled, session,
                            deadline](bool open) mutable {
                             if (!open) {
                               (*finish)(Error("amp direct chat: channel open failed")
                                             .WithUser(
                                                 "Direct send didn't confirm — will use relay if available."));
                               return;
                             }
                             auto* link = links_.FindLink(peer_key);
                             if (!link || !link->Mux() ||
                                 link->Mux()->State(channel_id) != pp::amp::ChannelState::Open) {
                               (*finish)(Error("amp direct chat: channel open failed")
                                             .WithUser(
                                                 "Direct send didn't confirm — will use relay if available."));
                               return;
                             }

                             session->Bind(*link->Mux(), channel_id, pp::amp::ControlJsonChannelPolicy(),
                                           [finish](Roe<std::vector<uint8_t>> ack) {
                                             if (!ack) {
                                               (*finish)(
                                                   Error("Failed to read direct chat ack")
                                                       .WithUser("Direct send didn't confirm — will use "
                                                                 "relay if available."));
                                               return false;
                                             }
                                             (*finish)({});
                                             return false;
                                           });

                             if (!session->EnqueueOutbound(JsonToBody(envelope_json))) {
                               (*finish)(Error("Failed to send direct chat envelope")
                                             .WithUser(
                                                 "Direct send didn't confirm — will use relay if available."));
                               return;
                             }
                             if (io_pump_) {
                               io_pump_();
                             }
                             if (post_io_) {
                               auto poll = std::make_shared<std::function<void()>>();
                               *poll = [this, finish, deadline, settled, poll]() {
                                 if (settled->load(std::memory_order_acquire)) {
                                   return;
                                 }
                                 if (Clock::now() >= deadline) {
                                   (*finish)(Error("amp direct chat send timed out")
                                                 .WithUser("Direct send didn't confirm — will use relay if "
                                                           "available."));
                                   return;
                                 }
                                 post_io_([poll, settled]() {
                                   if (!settled->load(std::memory_order_acquire)) {
                                     (*poll)();
                                   }
                                 });
                               };
                               post_io_([poll]() { (*poll)(); });
                             }
                           },
                           [this]() { return impl_->stopped.load(std::memory_order_acquire); });
                     });
}

Roe<void> AmpDirectChatTransport::SendEnvelope(const std::string& peer_relay_user_id,
                                               const RelayEnvelope& envelope) {
  auto result_promise = std::make_shared<std::promise<Roe<void>>>();
  auto result_future = result_promise->get_future();
  constexpr auto kSendTimeout = std::chrono::milliseconds(4000);
  const auto deadline = Clock::now() + kSendTimeout;

  SendEnvelopeAsync(peer_relay_user_id, envelope, [result_promise](Roe<void> value) {
    try {
      result_promise->set_value(std::move(value));
    } catch (const std::future_error&) {
    }
  });

  AmpParkUntil([&] { return result_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; },
               deadline, io_pump_);

  if (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    return Error("amp direct chat send timed out")
        .WithUser("Direct send didn't confirm — will use relay if available.");
  }
  return result_future.get();
}

} // namespace pbr
