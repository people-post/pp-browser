#include "domain/delivery/DeliveryOutbox.h"

#include "foundation/runtime/AppRuntime.h"
#include "lib/messaging/OutboundMessage.h"
#include "lib/net/OrgBackendClients.h"
#include "common/Utilities.h"

#include <algorithm>
#include <iterator>
#include <memory>

namespace pbr {

DeliveryOutbox::DeliveryOutbox(std::function<IDirectMessageClient*()> direct_chat)
    : direct_chat_(std::move(direct_chat)) {
  redirectLogger("DeliveryOutbox");
}

void DeliveryOutbox::Report(DeliveryOutcome outcome) {
  if (on_outcome_) {
    on_outcome_(outcome);
  }
}

void DeliveryOutbox::Enqueue(Pending pending) {
  std::lock_guard lock(mutex_);
  queue_.push_back(std::move(pending));
}

void DeliveryOutbox::Send(DeliveryRequest request) {
  const bool critical = request.critical_lane;
  auto work = [this, request = std::move(request)]() mutable {
    auto post_continue = [critical = request.critical_lane](std::function<void()> fn) {
      if (critical) {
        AppRuntime::PostWorkerCritical(std::move(fn));
      } else {
        AppRuntime::PostWorkerNormal(std::move(fn));
      }
    };
    // `direct`: the direct send's failure when it was tried — what the notice says when the relay
    // cannot carry the envelope either.
    auto finish_relay = [this, request](std::optional<DirectSendFailure> direct) {
      DeliveryOutcome outcome{.thread_id = request.thread_id, .message_id = request.message_id,
                              .group_member = request.group_member};
      outcome.relay_after_direct = direct.has_value();
      if (!request.relay_route) {
        outcome.failure = direct ? SendFailureFromDirect(direct->GetCode()) : SendFailure::NoAddress;
        outcome.detail = direct ? direct->message : "Direct thread missing peer relay id";
        Report(std::move(outcome));
        return;
      }
      IRelayClient* relay = relay_;
      if (!relay) {
        outcome.failure = direct ? SendFailureFromDirect(direct->GetCode()) : SendFailure::RelayNotConfigured;
        outcome.detail = direct ? direct->message : "Relay client not configured";
        Report(std::move(outcome));
        return;
      }
      if (auto sent = relay->Send(request.envelope); !sent) {
        log().warning << "relay send failed message_id=" << request.message_id << " peer=" << *request.relay_route
                      << " err=" << sent.error().message << " (queued for retry)";
        Enqueue(Pending{.envelope = request.envelope, .message_id = request.message_id,
                        .thread_id = request.thread_id, .attempt_count = 1,
                        .amp_peer_key = request.amp_peer_key, .group_member = request.group_member});
        outcome.failure = SendFailureFromRelay(sent.error());
        outcome.detail = sent.error().message;
        Report(std::move(outcome));
        return;
      }
      outcome.ok = true;
      outcome.transport = MessageTransport::Relay;
      Report(std::move(outcome));
    };

    IDirectMessageClient* chat = direct_chat_ ? direct_chat_() : nullptr;
    const bool reachable =
        chat && !request.amp_peer_key.empty() && chat->IsPeerReachable(request.amp_peer_key);
    // Call signaling: Amp's open + ack per message races media setup — the relay first when a
    // route is known; Amp remains for Amp-only peers.
    if (!DirectSendTriesAmpFirst(reachable, request.relay_route.has_value(), request.critical_lane)) {
      finish_relay(std::nullopt);
      return;
    }
    // Fire and forget: this worker is free while the mesh pump drives the open + ack.
    chat->SendEnvelopeAsync(
        request.amp_peer_key, request.envelope,
        [this, request, post_continue, finish_relay = std::move(finish_relay)](DirectSendRoe direct) mutable {
          post_continue([this, request, direct = std::move(direct), finish_relay = std::move(finish_relay)]() mutable {
            if (direct) {
              Report(DeliveryOutcome{.thread_id = request.thread_id, .message_id = request.message_id, .ok = true,
                                     .transport = MessageTransport::Direct, .group_member = request.group_member});
              return;
            }
            log().info << "direct send failed message_id=" << request.message_id << " peer=" << request.amp_peer_key
                       << " err=" << direct.error().message << " (relay next when routed)";
            finish_relay(direct.error());
          });
        });
  };
  if (critical) {
    AppRuntime::PostWorkerCritical(std::move(work));
  } else {
    AppRuntime::PostWorkerNormal(std::move(work));
  }
}

void DeliveryOutbox::Cancel(const std::string& message_id) {
  std::lock_guard lock(mutex_);
  std::erase_if(queue_, [&](const Pending& pending) { return pending.message_id == message_id; });
}

void DeliveryOutbox::PurgeThread(const std::string& thread_id) {
  std::lock_guard lock(mutex_);
  std::erase_if(queue_, [&](const Pending& pending) { return pending.thread_id == thread_id; });
}

void DeliveryOutbox::Retry() {
  std::vector<Pending> pending;
  {
    std::lock_guard lock(mutex_);
    pending.swap(queue_);
  }
  const int64_t now_ms = util::NowUnixMs();
  std::erase_if(pending, [this, now_ms](const Pending& item) {
    const bool control_lane = item.thread_id.empty();
    return !OutboxSendStillWanted(control_lane, now_ms - item.envelope.timestamp, Paused(item.thread_id));
  });
  if (pending.empty()) {
    return;
  }
  AppRuntime::PostWorkerNormal([this, pending = std::move(pending)]() mutable {
    if (!relay_) {
      std::lock_guard lock(mutex_);
      queue_.insert(queue_.end(), std::make_move_iterator(pending.begin()), std::make_move_iterator(pending.end()));
      return;
    }
    auto still_pending = std::make_shared<std::vector<Pending>>();
    auto process = std::make_shared<std::function<void(size_t)>>();
    *process = [this, pending = std::move(pending), still_pending, process](size_t index) mutable {
      if (index >= pending.size()) {
        if (!still_pending->empty()) {
          std::lock_guard lock(mutex_);
          queue_.insert(queue_.end(), std::make_move_iterator(still_pending->begin()),
                        std::make_move_iterator(still_pending->end()));
        }
        return;
      }
      Pending item = std::move(pending[index]);
      if (Paused(item.thread_id)) {
        (*process)(index + 1);
        return;
      }
      auto try_relay = [this, still_pending, process, index](Pending item, bool tried_direct) mutable {
        IRelayClient* relay = relay_;
        auto result = relay ? relay->Send(item.envelope) : Roe<void>(Error("Relay client not configured"));
        if (result) {
          Report(DeliveryOutcome{.thread_id = item.thread_id, .message_id = item.message_id, .ok = true,
                                 .transport = MessageTransport::Relay, .relay_after_direct = tried_direct,
                                 .group_member = item.group_member});
        } else if (OutboxRetryAgain(item.attempt_count)) {
          item.attempt_count += 1;
          still_pending->push_back(std::move(item));
        } else {
          Report(DeliveryOutcome{.thread_id = item.thread_id, .message_id = item.message_id,
                                 .failure = SendFailureFromRelay(result.error()), .detail = result.error().message,
                                 .group_member = item.group_member});
        }
        (*process)(index + 1);
      };
      IDirectMessageClient* chat = direct_chat_ ? direct_chat_() : nullptr;
      const std::string peer_key =
          !item.amp_peer_key.empty() ? item.amp_peer_key : item.envelope.recipient_contact_id.value_or("");
      if (!peer_key.empty() && chat && chat->IsPeerReachable(peer_key)) {
        const RelayEnvelope envelope = item.envelope;
        chat->SendEnvelopeAsync(
            peer_key, envelope,
            [this, process, index, item = std::move(item), try_relay = std::move(try_relay)](DirectSendRoe direct) mutable {
              AppRuntime::PostWorkerNormal([this, process, index, item = std::move(item), try_relay = std::move(try_relay),
                                            direct = std::move(direct)]() mutable {
                if (direct) {
                  Report(DeliveryOutcome{.thread_id = item.thread_id, .message_id = item.message_id, .ok = true,
                                         .transport = MessageTransport::Direct, .group_member = item.group_member});
                  (*process)(index + 1);
                  return;
                }
                try_relay(std::move(item), true);
              });
            });
        return;
      }
      try_relay(std::move(item), false);
    };
    (*process)(0);
  });
}

} // namespace pbr
