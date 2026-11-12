#include "domain/delivery/DeliveryOutbox.h"

#include "common/runtime/AppRuntime.h"
#include "common/PbrCompat.h"
#include "lib/delivery/OutboundMessage.h"
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

DeliveryOutbox::~DeliveryOutbox() {
  std::vector<uint64_t> timers;
  {
    std::lock_guard lock(mutex_);
    for (const auto& entry : control_retries_) {
      timers.push_back(entry.second.timer_id);
    }
    control_retries_.clear();
  }
  for (const uint64_t timer : timers) {
    AppRuntime::CancelCoordinatorTimer(timer);
  }
  lifetime_.InvalidateAndWait();
}

void DeliveryOutbox::Report(DeliveryOutcome outcome) {
  if (on_outcome_) {
    on_outcome_(outcome);
  }
}

void DeliveryOutbox::Finish(DeliveryOutcome outcome) {
  if (!outcome.thread_id.empty()) {
    std::lock_guard lock(mutex_);
    EndActiveLocked(outcome.message_id);
  }
  Report(std::move(outcome));
}

uint64_t DeliveryOutbox::BeginActive(const std::string& thread_id, const std::string& message_id) {
  std::lock_guard lock(mutex_);
  const uint64_t serial = ++next_serial_;
  if (thread_id.empty() || message_id.empty()) {
    return serial;
  }
  ActiveSend& active = active_[message_id];
  if (active.requests == 0) {
    active.started_ms = util::NowUnixMs();
  }
  ++active.requests;
  return serial;
}

void DeliveryOutbox::EndActiveLocked(const std::string& message_id) {
  const auto it = active_.find(message_id);
  if (it == active_.end()) {
    return;
  }
  if (--it->second.requests <= 0) {
    active_.erase(it);
    cancelled_.erase(message_id);
  }
}

bool DeliveryOutbox::CancelledLocked(const std::string& thread_id, const std::string& message_id,
                                     const uint64_t serial) const {
  if (thread_id.empty()) {
    return false;
  }
  const auto it = cancelled_.find(message_id);
  return it != cancelled_.end() && serial <= it->second;
}

bool DeliveryOutbox::EndIfCancelled(const std::string& thread_id, const std::string& message_id,
                                    const uint64_t serial) {
  std::lock_guard lock(mutex_);
  if (!CancelledLocked(thread_id, message_id, serial)) {
    return false;
  }
  log().info << "send cancelled message_id=" << message_id << " (no further attempt)";
  EndActiveLocked(message_id);
  return true;
}

std::optional<int64_t> DeliveryOutbox::SendStartedMs(const std::string& message_id) const {
  std::lock_guard lock(mutex_);
  const auto it = active_.find(message_id);
  if (it == active_.end()) {
    return std::nullopt;
  }
  return it->second.started_ms;
}

bool DeliveryOutbox::Enqueue(Pending pending) {
  std::lock_guard lock(mutex_);
  return EnqueueLocked(std::move(pending));
}

bool DeliveryOutbox::EnqueueLocked(Pending pending) {
  if (CancelledLocked(pending.thread_id, pending.message_id, pending.serial)) {
    log().info << "send cancelled message_id=" << pending.message_id << " (not queued again)";
    EndActiveLocked(pending.message_id);
    return false;
  }
  queue_.push_back(std::move(pending));
  return true;
}

void DeliveryOutbox::Send(DeliveryRequest request) {
  const bool critical = request.critical_lane;
  const uint64_t serial = BeginActive(request.thread_id, request.message_id);
  auto work = [this, serial, request = std::move(request)]() mutable {
    if (EndIfCancelled(request.thread_id, request.message_id, serial)) {
      return;
    }
    auto post_continue = [critical = request.critical_lane](std::function<void()> fn) {
      if (critical) {
        AppRuntime::PostWorkerCritical(std::move(fn));
      } else {
        AppRuntime::PostWorkerNormal(std::move(fn));
      }
    };
    // `direct`: the direct send's failure when it was tried — what the notice says when the relay
    // cannot carry the envelope either.
    auto finish_relay = [this, serial, request](std::optional<DirectSendFailure> direct) {
      // After a direct failure the relay is a new attempt.
      if (direct && EndIfCancelled(request.thread_id, request.message_id, serial)) {
        return;
      }
      DeliveryOutcome outcome{.thread_id = request.thread_id,
                              .message_id = request.message_id,
                              .group_member = request.group_member,
                              .copy_device = request.copy_device};
      if (!request.relay_route) {
        outcome.failure = direct ? SendFailureFromDirect(direct->GetCode()) : SendFailure::NoAddress;
        outcome.detail = direct ? direct->message : "Direct thread missing peer relay id";
        Finish(std::move(outcome));
        return;
      }
      IRelayClient* relay = relay_;
      if (!relay) {
        outcome.failure = direct ? SendFailureFromDirect(direct->GetCode()) : SendFailure::RelayNotConfigured;
        outcome.detail = direct ? direct->message : "Relay client not configured";
        Finish(std::move(outcome));
        return;
      }
      if (auto sent = relay->Send(request.envelope); !sent) {
        Pending pending{.envelope = request.envelope,
                        .message_id = request.message_id,
                        .thread_id = request.thread_id,
                        .attempt_count = 1,
                        .amp_peer_key = request.amp_peer_key,
                        .group_member = request.group_member,
                        .copy_device = request.copy_device,
                        .serial = serial};
        const bool control_lane = request.thread_id.empty();
        bool again = false;
        if (control_lane) {
          again = RetryControlLater(std::move(pending), sent.error());
        } else if ((again = OutboxRetryAgain(1, sent.error())) && !Enqueue(std::move(pending))) {
          return; // Cancelled while it was on the way: it ends here.
        }
        log().warning << "relay send failed message_id=" << request.message_id << " peer=" << *request.relay_route
                      << " err=" << sent.error().message
                      << (!again         ? " (not retried)"
                          : control_lane ? " (retrying shortly)"
                                         : " (queued for retry)");
        outcome.failure = SendFailureFromRelay(sent.error());
        outcome.detail = sent.error().message;
        // Queued for a retry: the send goes on, so the failure is reported without ending it.
        if (again) {
          Report(std::move(outcome));
        } else {
          Finish(std::move(outcome));
        }
        return;
      }
      outcome.ok = true;
      outcome.transport = MessageTransport::Relay;
      Finish(std::move(outcome));
    };

    IDirectMessageClient* chat = direct_chat_ ? direct_chat_() : nullptr;
    const bool reachable = chat && !request.amp_peer_key.empty() && chat->IsPeerReachable(request.amp_peer_key);
    const bool connected = chat && !request.amp_peer_key.empty() && chat->IsPeerConnected(request.amp_peer_key);
    if (ControlAlsoGoesDirect(connected, request.relay_route.has_value(), request.critical_lane)) {
      // #431 (a): a live link carries it in milliseconds where the inbox can take tens of seconds; the
      // relay copy still goes (the outcome is its), and the receiver applies whichever lands first.
      chat->SendEnvelopeAsync(
          request.amp_peer_key, request.envelope,
          [this, message_id = request.message_id, peer = request.amp_peer_key](DirectSendRoe direct) {
            if (!direct) {
              log().info << "control direct copy failed message_id=" << message_id << " peer=" << peer
                         << " err=" << direct.error().message << " (the relay copy stands)";
            }
          });
      finish_relay(std::nullopt);
      return;
    }
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
              Finish(DeliveryOutcome{.thread_id = request.thread_id,
                                     .message_id = request.message_id,
                                     .ok = true,
                                     .transport = MessageTransport::Direct,
                                     .group_member = request.group_member,
                                     .copy_device = request.copy_device});
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

bool DeliveryOutbox::RetryControlLater(Pending item, const Error& failure) {
  const auto delay = ControlRetryAfter(item.attempt_count, util::NowUnixMs() - item.envelope.timestamp, failure);
  if (!delay) {
    return false;
  }
  uint64_t seq = 0;
  {
    std::lock_guard lock(mutex_);
    seq = ++next_control_retry_;
  }
  const std::string message_id = item.message_id;
  const uint64_t timer =
      AppRuntime::ScheduleCoordinatorOneShot(*delay, lifetime_.Bind([this, seq, item = std::move(item)]() mutable {
        {
          std::lock_guard lock(mutex_);
          if (control_retries_.erase(seq) == 0) {
            return; // Cancelled as it fired.
          }
        }
        AppRuntime::PostWorkerCritical(
            lifetime_.Bind([this, item = std::move(item)]() mutable { RetryControlNow(std::move(item)); }));
      }));
  // Recorded after scheduling (the timer's lock is not taken under ours); the delay is a second at least.
  std::lock_guard lock(mutex_);
  control_retries_[seq] = ControlRetry{.timer_id = timer, .message_id = message_id};
  return true;
}

void DeliveryOutbox::RetryControlNow(Pending item) {
  IRelayClient* relay = relay_;
  if (!relay) {
    Enqueue(std::move(item));
    return;
  }
  auto sent = relay->Send(item.envelope);
  DeliveryOutcome outcome{.thread_id = item.thread_id,
                          .message_id = item.message_id,
                          .group_member = item.group_member,
                          .copy_device = item.copy_device};
  if (sent) {
    log().info << "control retry delivered message_id=" << item.message_id << " attempt=" << item.attempt_count + 1;
    outcome.ok = true;
    outcome.transport = MessageTransport::Relay;
    Report(std::move(outcome));
    return;
  }
  item.attempt_count += 1;
  const int attempts = item.attempt_count;
  if (RetryControlLater(std::move(item), sent.error())) {
    return;
  }
  log().warning << "control retry gave up message_id=" << outcome.message_id << " attempts=" << attempts
                << " err=" << sent.error().message;
  outcome.failure = SendFailureFromRelay(sent.error());
  outcome.detail = sent.error().message;
  Report(std::move(outcome));
}

void DeliveryOutbox::Cancel(const std::string& message_id) {
  std::vector<uint64_t> timers;
  {
    std::lock_guard lock(mutex_);
    std::erase_if(queue_, [&](const Pending& pending) {
      if (pending.message_id != message_id) {
        return false;
      }
      if (!pending.thread_id.empty()) {
        EndActiveLocked(pending.message_id);
      }
      return true;
    });
    if (active_.contains(message_id)) {
      cancelled_[message_id] = next_serial_; // what is still on its way (or held by a Retry) goes no further
    }
    std::erase_if(control_retries_, [&](const auto& entry) {
      if (entry.second.message_id != message_id) {
        return false;
      }
      timers.push_back(entry.second.timer_id);
      return true;
    });
  }
  for (const uint64_t timer : timers) {
    AppRuntime::CancelCoordinatorTimer(timer);
  }
}

void DeliveryOutbox::PurgeThread(const std::string& thread_id) {
  std::lock_guard lock(mutex_);
  std::erase_if(queue_, [&](const Pending& pending) {
    if (pending.thread_id != thread_id) {
      return false;
    }
    if (!pending.thread_id.empty()) {
      EndActiveLocked(pending.message_id);
    }
    return true;
  });
}

void DeliveryOutbox::Retry() {
  std::vector<Pending> pending;
  {
    std::lock_guard lock(mutex_);
    pending.swap(queue_);
  }
  const int64_t now_ms = util::NowUnixMs();
  std::vector<std::string> dropped; // rows whose request ends here (not sent again)
  std::erase_if(pending, [this, now_ms, &dropped](const Pending& item) {
    const bool control_lane = item.thread_id.empty();
    if (OutboxSendStillWanted(control_lane, now_ms - item.envelope.timestamp, Paused(item.thread_id))) {
      return false;
    }
    if (!control_lane) {
      dropped.push_back(item.message_id);
    }
    return true;
  });
  if (!dropped.empty()) {
    std::lock_guard lock(mutex_);
    for (const std::string& message_id : dropped) {
      EndActiveLocked(message_id);
    }
  }
  if (pending.empty()) {
    return;
  }
  AppRuntime::PostWorkerNormal([this, pending = std::move(pending)]() mutable {
    if (!relay_) {
      std::lock_guard lock(mutex_);
      for (Pending& item : pending) {
        EnqueueLocked(std::move(item));
      }
      return;
    }
    auto still_pending = std::make_shared<std::vector<Pending>>();
    auto process = std::make_shared<std::function<void(size_t)>>();
    *process = [this, pending = std::move(pending), still_pending, process](size_t index) mutable {
      if (index >= pending.size()) {
        if (!still_pending->empty()) {
          std::lock_guard lock(mutex_);
          for (Pending& item : *still_pending) {
            EnqueueLocked(std::move(item));
          }
        }
        return;
      }
      Pending item = std::move(pending[index]);
      if (Paused(item.thread_id)) {
        {
          std::lock_guard lock(mutex_); // dropped, not queued again: its request ends
          EndActiveLocked(item.message_id);
        }
        (*process)(index + 1);
        return;
      }
      if (EndIfCancelled(item.thread_id, item.message_id, item.serial)) {
        (*process)(index + 1);
        return;
      }
      auto try_relay = [this, still_pending, process, index](Pending item) mutable {
        if (EndIfCancelled(item.thread_id, item.message_id, item.serial)) {
          (*process)(index + 1);
          return;
        }
        IRelayClient* relay = relay_;
        auto result = relay ? relay->Send(item.envelope) : Roe<void>(Error("Relay client not configured"));
        if (result) {
          Finish(DeliveryOutcome{.thread_id = item.thread_id,
                                 .message_id = item.message_id,
                                 .ok = true,
                                 .transport = MessageTransport::Relay,
                                 .group_member = item.group_member,
                                 .copy_device = item.copy_device});
        } else if (OutboxRetryAgain(item.attempt_count, result.error())) {
          item.attempt_count += 1;
          still_pending->push_back(std::move(item));
        } else {
          Finish(DeliveryOutcome{.thread_id = item.thread_id,
                                 .message_id = item.message_id,
                                 .failure = SendFailureFromRelay(result.error()),
                                 .detail = result.error().message,
                                 .group_member = item.group_member,
                                 .copy_device = item.copy_device});
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
            [this, process, index, item = std::move(item),
             try_relay = std::move(try_relay)](DirectSendRoe direct) mutable {
              AppRuntime::PostWorkerNormal([this, process, index, item = std::move(item),
                                            try_relay = std::move(try_relay), direct = std::move(direct)]() mutable {
                if (direct) {
                  Finish(DeliveryOutcome{.thread_id = item.thread_id,
                                         .message_id = item.message_id,
                                         .ok = true,
                                         .transport = MessageTransport::Direct,
                                         .group_member = item.group_member,
                                         .copy_device = item.copy_device});
                  (*process)(index + 1);
                  return;
                }
                try_relay(std::move(item));
              });
            });
        return;
      }
      try_relay(std::move(item));
    };
    (*process)(0);
  });
}

} // namespace pbr
