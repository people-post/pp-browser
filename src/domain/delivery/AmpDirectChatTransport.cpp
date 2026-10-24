#include "domain/delivery/AmpDirectChatTransport.h"

#include "mesh/l4/shared/InboundReply.h"

#include "common/chat/MessagingJson.h"
#include "common/chat/MessagingLimits.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/link/LinkIdentity.h"
#include "mesh/shared/AmpParkUntil.h"
#include "common/Lifetime.h"

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <vector>
#include "common/ValueJson.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

namespace {

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

int64_t SteadyDeadlineMs(const Clock::time_point deadline) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(deadline.time_since_epoch()).count();
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
  mesh::IChatPeerLinks* links = nullptr;
  IoPump io_pump;
  WorkerPost post_worker;
  IoPost post_io;
  std::mutex handler_mutex;
  InboundHandler inbound;
  std::atomic<bool> stopped{false};

  /** Guards protocol-handler raw Impl* past Stop — OWNERSHIP.md § Lifetime. */
  pp::Lifetime deferred;
  void HandleInboundChannel(pp::amp::LinkHandle link, const std::string& remote_peer_id, const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || !links || remote_peer_id.empty()) {
      return;
    }
    auto session_holder = std::make_shared<std::shared_ptr<pp::amp::ChannelSession>>();
    auto policy = mesh::InboundReplyPolicy(pp::amp::ControlJsonChannelPolicy());
    *session_holder = links->BindChannelOnLink(link, channel_id, policy,
        [this, session_holder, remote_peer_id](Roe<std::vector<uint8_t>> frame) {
          auto session = *session_holder;
          if (!session || !frame || stopped.load(std::memory_order_acquire)) {
            return false;
          }
          auto body = std::move(*frame);
          // Keep the channel open for the worker's ack (InboundReply.h); `reply` closes it.
          auto reply = mesh::MakeInboundReply(session, post_io);
          RunWorker(post_worker, [this, reply, remote_peer_id, body = std::move(body)]() mutable {
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
            reply->Send(JsonToBody(kAck));
            reply->Close();
            if (handler) {
              handler(std::move(*envelope), remote_peer_id);
            }
          });
          return true;
        });
  }
};

AmpDirectChatTransport::AmpDirectChatTransport(mesh::IChatPeerLinks& links, IoPump io_pump, WorkerPost post_worker,
                                               IoPost post_io, IoAfter post_after)
    : impl_(std::make_unique<Impl>()), links_(links), io_pump_(std::move(io_pump)),
      post_worker_(std::move(post_worker)), post_io_(std::move(post_io)),
      post_after_(std::move(post_after)) {
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
  links_.SetProtocolHandler(
      kDirectChatProtocolId,
      impl_->deferred.Bind([impl = impl_.get()](pp::amp::LinkHandle handle,
                                                const std::string& remote_peer_id,
                                                const uint32_t channel_id) {
        impl->HandleInboundChannel(handle, remote_peer_id, channel_id);
      }));
}

void AmpDirectChatTransport::Stop() {
  // Idempotent: the destructor Stops again, possibly after MeshHost::Stop freed the runtime.
  if (!started_) {
    return;
  }
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  links_.RemoveProtocolHandler(kDirectChatProtocolId);
  impl_->deferred.Invalidate();
  std::lock_guard lock(impl_->handler_mutex);
  impl_->inbound = nullptr;
}

void AmpDirectChatTransport::SetInboundHandler(InboundHandler handler) {
  std::lock_guard lock(impl_->handler_mutex);
  impl_->inbound = std::move(handler);
}

DirectSendFailure AmpDirectChatTransport::WrapLinkFailure(const mesh::IChatPeerLinks::Failure& link) {
  using LinkErr = mesh::IChatPeerLinks::Err;
  DirectSendErr code = DirectSendErr::Generic;
  switch (link.GetCode()) {
  case LinkErr::EndpointNotRegistered:
    code = DirectSendErr::NoAddress;
    break;
  case LinkErr::DialInBackoff:
    code = DirectSendErr::DialInBackoff;
    break;
  case LinkErr::TooManyConcurrentDials:
  case LinkErr::MaxLinksReached:
    code = DirectSendErr::DialBusy;
    break;
  case LinkErr::DialTimeout:
    code = DirectSendErr::NoAnswer;
    break;
  case LinkErr::HandshakeFailed:
    code = DirectSendErr::Refused;
    break;
  case LinkErr::TransportFailed:
    code = DirectSendErr::TransportFailed;
    break;
  case LinkErr::ChannelOpenFailed:
  case LinkErr::AssociationNotReady:
  case LinkErr::LinkNotFound:
  case LinkErr::NestedCarrierIncomplete:
    code = DirectSendErr::ChannelFailed;
    break;
  default:
    code = DirectSendErr::Generic;
    break;
  }
  return DirectSendFailure::Of(code, detail::AppendFrom("amp direct chat: link failed", "link", link.message));
}

void AmpDirectChatTransport::SetDevicesOf(DevicesOf devices_of) {
  devices_of_ = std::move(devices_of);
}

std::string AmpDirectChatTransport::ReachableKey(const std::string& key) const {
  const auto reaches = [this](const std::string& k) {
    return links_.IsReachable(k) || links_.GetLinkSnapshot(k).has_endpoint || links_.IsConnected(k);
  };
  if (key.empty() || reaches(key)) {
    return key.empty() ? std::string() : key;
  }
  if (!devices_of_) {
    return {};
  }
  const std::vector<std::string> devices = devices_of_(key);
  for (const std::string& device : devices) {
    if (links_.IsConnected(device)) {
      return device;
    }
  }
  for (const std::string& device : devices) {
    if (reaches(device)) {
      return device;
    }
  }
  return {};
}

bool AmpDirectChatTransport::IsPeerReachable(const std::string& peer_identity_value) const {
  return !ReachableKey(peer_identity_value).empty();
}

void AmpDirectChatTransport::SendEnvelopeAsync(const std::string& peer_relay_user_id,
                                               const RelayEnvelope& envelope,
                                               std::function<void(DirectSendRoe)> on_done) {
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto finish_once = std::make_shared<std::function<void(DirectSendRoe)>>();
  *finish_once = [on_done = std::move(on_done), settled](DirectSendRoe value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    if (on_done) {
      on_done(std::move(value));
    }
  };

  if (!started_) {
    (*finish_once)(DirectSendFailure::Of(DirectSendErr::NotStarted, "amp direct chat service not started"));
    return;
  }
  if (!IsPeerReachable(peer_relay_user_id)) {
    (*finish_once)(DirectSendFailure::Of(DirectSendErr::NoAddress, "Peer-direct endpoint not registered"));
    return;
  }

  const std::string envelope_json = DumpJson(RelayEnvelopeToJson(envelope));
  constexpr auto kSendTimeout = std::chrono::milliseconds(4000);
  const auto deadline = Clock::now() + kSendTimeout;
  auto session_holder = std::make_shared<std::shared_ptr<pp::amp::ChannelSession>>();
  auto finish = std::make_shared<std::function<void(DirectSendRoe)>>();
  *finish = [finish_once, session_holder](DirectSendRoe value) {
    if (*session_holder) {
      (*session_holder)->Close();
    }
    (*finish_once)(std::move(value));
  };

  const std::string peer_key = ReachableKey(peer_relay_user_id);
  links_.OpenChannel(peer_key, kDirectChatProtocolId, pp::amp::ControlJsonChannelPolicy(),
                     [this, peer_key, envelope_json, finish, settled, session_holder,
                      deadline](mesh::IChatPeerLinks::ChannelRoe channel) mutable {
                       if (!channel) {
                         (*finish)(WrapLinkFailure(channel.error()));
                         return;
                       }
                       if (impl_->stopped.load(std::memory_order_acquire)) {
                         (*finish)(DirectSendFailure::Of(DirectSendErr::NotStarted, "amp direct chat service stopped"));
                         return;
                       }
                       const uint32_t channel_id = *channel;
                       links_.WhenChannelOpen(
                           peer_key, channel_id, SteadyDeadlineMs(deadline),
                           [this, peer_key, channel_id, envelope_json, finish, settled, session_holder,
                            deadline](bool open) mutable {
                             if (impl_->stopped.load(std::memory_order_acquire)) {
                               (*finish)(DirectSendFailure::Of(DirectSendErr::NotStarted, "amp direct chat service stopped"));
                               return;
                             }
                             if (!open) {
                               (*finish)(DirectSendFailure::Of(DirectSendErr::ChannelFailed,
                                                               "amp direct chat: channel open failed"));
                               return;
                             }

                             *session_holder = links_.BindChannel(
                                 peer_key, channel_id, pp::amp::ControlJsonChannelPolicy(),
                                 [finish](Roe<std::vector<uint8_t>> ack) {
                                   if (!ack) {
                                     (*finish)(DirectSendFailure::Of(DirectSendErr::NotConfirmed,
                                                                     "Failed to read direct chat ack"));
                                     return false;
                                   }
                                   (*finish)(DirectSendRoe());
                                   return false;
                                 });
                             if (!*session_holder) {
                               (*finish)(DirectSendFailure::Of(DirectSendErr::ChannelFailed,
                                                               "amp direct chat: channel open failed"));
                               return;
                             }

                             if (!(*session_holder)->EnqueueOutbound(JsonToBody(envelope_json))) {
                               (*finish)(DirectSendFailure::Of(DirectSendErr::ChannelFailed,
                                                               "Failed to send direct chat envelope"));
                               return;
                             }
                             mesh::AmpScheduleUntilSettled(post_io_, io_pump_, settled, deadline, [finish]() {
                               (*finish)(DirectSendFailure::Of(DirectSendErr::NotConfirmed,
                                                               "amp direct chat send timed out"));
                             },
                                                    post_after_);
                           });
                     });
}

DirectSendRoe AmpDirectChatTransport::SendEnvelope(const std::string& peer_relay_user_id,
                                                   const RelayEnvelope& envelope) {
  auto result_promise = std::make_shared<std::promise<DirectSendRoe>>();
  auto result_future = result_promise->get_future();
  constexpr auto kSendTimeout = std::chrono::milliseconds(4000);
  const auto deadline = Clock::now() + kSendTimeout;

  SendEnvelopeAsync(peer_relay_user_id, envelope, [result_promise](DirectSendRoe value) {
    try {
      result_promise->set_value(std::move(value));
    } catch (const std::future_error&) {
    }
  });

  mesh::AmpParkUntil([&] { return result_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; },
               deadline, io_pump_);

  if (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    return DirectSendFailure::Of(DirectSendErr::NotConfirmed, "amp direct chat send timed out");
  }
  return result_future.get();
}

} // namespace pbr
