#include "domain/delivery/AmpAccountCardTransport.h"

#include "mesh/l4/shared/InboundReply.h"
#include "mesh/shared/AmpParkUntil.h"

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/link/LinkIdentity.h"

#include "common/Lifetime.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;
/** A card is about 14 KB: one round trip on a live link. */
constexpr auto kFetchTimeout = std::chrono::milliseconds(6000);

int64_t SteadyDeadlineMs(const Clock::time_point deadline) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(deadline.time_since_epoch()).count();
}

std::vector<uint8_t> Body(const std::string& json) { return std::vector<uint8_t>(json.begin(), json.end()); }

} // namespace

struct AmpAccountCardTransport::Impl {
  mesh::IChatPeerLinks* links = nullptr;
  WorkerPost post_worker;
  IoPost post_io;
  std::mutex answer_mutex;
  Answer answer;
  std::atomic<bool> stopped{false};
  /** Guards protocol-handler raw Impl* past Stop — OWNERSHIP.md § Lifetime. */
  pp::Lifetime deferred;

  void HandleInboundChannel(pp::amp::LinkHandle link, const std::string& remote_peer_id, const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || !links || remote_peer_id.empty()) {
      return;
    }
    auto session_holder = std::make_shared<std::shared_ptr<pp::amp::ChannelSession>>();
    auto policy = mesh::InboundReplyPolicy(pp::amp::ControlJsonChannelPolicy());
    *session_holder = links->BindChannelOnLink(link, channel_id, policy, [this, session_holder, remote_peer_id](Roe<std::vector<uint8_t>> frame) {
          auto session = *session_holder;
          if (!session || !frame || stopped.load(std::memory_order_acquire)) {
            return false;
          }
          auto body = std::move(*frame);
          auto reply = mesh::MakeInboundReply(session, post_io);
          auto task = [this, reply, remote_peer_id, body = std::move(body)]() {
            if (stopped.load(std::memory_order_acquire)) {
              return;
            }
            AccountCardReply out{.error = "not_available"};
            auto request = DecodeAccountCardRequest(std::string(body.begin(), body.end()));
            Answer answer_fn;
            {
              std::lock_guard lock(answer_mutex);
              answer_fn = answer;
            }
            if (request && answer_fn) {
              out = answer_fn(*request, remote_peer_id);
            }
            reply->Send(Body(EncodeAccountCardReply(out)));
            reply->Close();
          };
          post_worker ? post_worker(std::move(task)) : task();
          return true;
        });
  }
};

AmpAccountCardTransport::AmpAccountCardTransport(mesh::IChatPeerLinks& links, IoPump io_pump, WorkerPost post_worker,
                                                 IoPost post_io, IoAfter post_after)
    : impl_(std::make_unique<Impl>()), links_(links), io_pump_(std::move(io_pump)), post_io_(std::move(post_io)),
      post_after_(std::move(post_after)) {
  impl_->links = &links_;
  impl_->post_worker = std::move(post_worker);
  impl_->post_io = post_io_;
}

AmpAccountCardTransport::~AmpAccountCardTransport() { Stop(); }

void AmpAccountCardTransport::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  impl_->stopped.store(false, std::memory_order_release);
  links_.SetProtocolHandler(kAccountCardProtocolId,
                            impl_->deferred.Bind([impl = impl_.get()](pp::amp::LinkHandle handle,
                                                                      const std::string& remote_peer_id,
                                                                      const uint32_t channel_id) {
                              impl->HandleInboundChannel(handle, remote_peer_id, channel_id);
                            }));
}

void AmpAccountCardTransport::Stop() {
  if (!started_) {
    return;
  }
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  links_.RemoveProtocolHandler(kAccountCardProtocolId);
  impl_->deferred.Invalidate();
}

void AmpAccountCardTransport::SetAnswer(Answer answer) {
  std::lock_guard lock(impl_->answer_mutex);
  impl_->answer = std::move(answer);
}

void AmpAccountCardTransport::FetchAsync(const std::string& peer_id, const AccountCardRequest& request,
                                         std::function<void(Roe<AccountCardReply>)> on_done) {
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto session_holder = std::make_shared<std::shared_ptr<pp::amp::ChannelSession>>();
  auto finish = [on_done = std::move(on_done), settled, session_holder](Roe<AccountCardReply> value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    if (*session_holder) {
      (*session_holder)->Close();
    }
    if (on_done) {
      on_done(std::move(value));
    }
  };
  if (!started_) {
    finish(Error("account card service not started"));
    return;
  }
  const auto deadline = Clock::now() + kFetchTimeout;
  links_.OpenChannel(
      peer_id, kAccountCardProtocolId, pp::amp::ControlJsonChannelPolicy(),
      [this, peer_id, json = EncodeAccountCardRequest(request), finish, settled, session_holder,
       deadline](mesh::IChatPeerLinks::ChannelRoe channel) mutable {
        if (!channel) {
          finish(Error(channel.error().message));
          return;
        }
        if (impl_->stopped.load(std::memory_order_acquire)) {
          finish(Error("account card service stopped"));
          return;
        }
        const uint32_t channel_id = *channel;
        links_.WhenChannelOpen(peer_id, channel_id, SteadyDeadlineMs(deadline),
                               [this, peer_id, channel_id, json, finish, settled, session_holder, deadline](bool open) {
                                 if (!open || impl_->stopped.load(std::memory_order_acquire)) {
                                   finish(Error("account card: channel did not open"));
                                   return;
                                 }
                                 *session_holder = links_.BindChannel(
                                     peer_id, channel_id, pp::amp::ControlJsonChannelPolicy(),
                                     [finish](Roe<std::vector<uint8_t>> frame) {
                                       if (!frame) {
                                         finish(Error("account card: no reply"));
                                         return false;
                                       }
                                       finish(DecodeAccountCardReply(std::string(frame->begin(), frame->end())));
                                       return false;
                                     });
                                 if (!(*session_holder)->EnqueueOutbound(Body(json))) {
                                   finish(Error("account card: request not sent"));
                                   return;
                                 }
                                 mesh::AmpScheduleUntilSettled(post_io_, io_pump_, settled, deadline,
                                                               [finish]() { finish(Error("account card: timed out")); },
                                                               post_after_);
                               });
      });
}

} // namespace pbr
