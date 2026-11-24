#include "domain/delivery/DeliveryOutbox.h"

#include "common/chat/RelayStreamKey.h"
#include "common/runtime/AppRuntime.h"
#include "common/PbrCompat.h"
#include "lib/delivery/OutboundMessage.h"
#include "lib/net/OrgBackendClients.h"
#include "common/Utilities.h"
#include "foundation/runtime/BackgroundActivity.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <unordered_set>

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

DeliveryOutcome DeliveryOutbox::RelayFailureOutcome(DeliveryOutcome outcome, const std::string& recipient,
                                                    const Error& failure) {
  outcome.ok = false;
  outcome.failure = SendFailureFromRelay(failure);
  outcome.detail = failure.message;
  outcome.relay_recipient = recipient;
  outcome.initiation_floor = RelayPaymentRequiredFloor(failure);
  outcome.stranger_quota = RelayStrangerQuota(failure);
  return outcome;
}

DeliveryOutcome DeliveryOutbox::DroppedOutcome(const Pending& item) {
  return RelayFailureOutcome(DeliveryOutcome{.thread_id = item.thread_id,
                                             .message_id = item.message_id,
                                             .group_member = item.group_member,
                                             .copy_device = item.copy_device},
                             item.relay_recipient, item.last_failure);
}

void DeliveryOutbox::Report(DeliveryOutcome outcome) {
  if (on_outcome_) {
    on_outcome_(outcome);
  }
}

void DeliveryOutbox::Finish(DeliveryOutcome outcome) {
  bool followers = false;
  const std::string thread_id = outcome.thread_id;
  if (!thread_id.empty()) {
    std::lock_guard lock(mutex_);
    EndActiveLocked(outcome.message_id);
    // Its chat's later messages that waited untried behind it go now, in order (B236).
    followers = !active_.contains(outcome.message_id) && outcome.group_member.empty() &&
                std::any_of(queue_.begin(), queue_.end(), [&](const Pending& item) {
                  return item.thread_id == thread_id && item.group_member.empty() && item.attempt_count == 0;
                });
  }
  Report(std::move(outcome));
  if (followers) {
    AppRuntime::PostWorkerNormal(lifetime_.Bind([this, thread_id]() { Retry(thread_id); }));
  }
}

uint64_t DeliveryOutbox::BeginActive(const std::string& thread_id, const std::string& message_id, const bool group) {
  std::lock_guard lock(mutex_);
  const uint64_t serial = ++next_serial_;
  if (thread_id.empty() || message_id.empty()) {
    return serial;
  }
  ActiveSend& active = active_[message_id];
  if (active.requests == 0) {
    active.started_ms = util::NowUnixMs();
    active.thread_id = thread_id;
    active.first_serial = serial;
    active.group = group;
  }
  ++active.requests;
  return serial;
}

bool DeliveryOutbox::WaitsBehindEarlierLocked(const std::string& thread_id, const std::string& message_id) const {
  const auto mine = active_.find(message_id);
  if (thread_id.empty() || mine == active_.end() || mine->second.group) {
    return false;
  }
  return std::any_of(active_.begin(), active_.end(), [&](const auto& entry) {
    return entry.first != message_id && !entry.second.group && entry.second.thread_id == thread_id &&
           entry.second.first_serial < mine->second.first_serial;
  });
}

std::string DeliveryOutbox::OrderLane(const Pending& item) {
  return item.group_member.empty() ? item.thread_id : item.thread_id + "|" + item.group_member;
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
  bool wake = false;
  {
    std::lock_guard lock(mutex_);
    if (!CancelledLocked(thread_id, message_id, serial)) {
      return false;
    }
    log().info << "send cancelled message_id=" << message_id << " (no further attempt)";
    EndActiveLocked(message_id);
    wake = FollowersFreeLocked(thread_id);
  }
  if (wake) {
    WakeFollowers({thread_id});
  }
  return true;
}

bool DeliveryOutbox::FollowersFreeLocked(const std::string& thread_id) const {
  if (thread_id.empty()) {
    return false;
  }
  const auto queued = [&](const std::string& message_id) {
    return std::any_of(queue_.begin(), queue_.end(),
                       [&](const Pending& item) { return item.message_id == message_id; });
  };
  const bool waiting = std::any_of(queue_.begin(), queue_.end(), [&](const Pending& item) {
    return item.thread_id == thread_id && item.group_member.empty() && item.attempt_count == 0;
  });
  return waiting && std::none_of(active_.begin(), active_.end(), [&](const auto& entry) {
           return !entry.second.group && entry.second.thread_id == thread_id && !queued(entry.first);
         });
}

void DeliveryOutbox::WakeFollowers(const std::vector<std::string>& thread_ids) {
  for (const std::string& thread_id : thread_ids) {
    AppRuntime::PostWorkerNormal(lifetime_.Bind([this, thread_id]() { Retry(thread_id); }));
  }
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
  const std::string thread_id = pending.thread_id;
  bool wake = false;
  {
    std::lock_guard lock(mutex_);
    if (EnqueueLocked(std::move(pending))) {
      return true;
    }
    wake = FollowersFreeLocked(thread_id); // cancelled on its way: what waited behind it goes
  }
  if (wake) {
    WakeFollowers({thread_id});
  }
  return false;
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
  const uint64_t serial = BeginActive(request.thread_id, request.message_id, !request.group_member.empty());
  if (!critical && request.relay_recipient) {
    // In a 1:1 chat messages arrive in the order they were sent (B236): one still on its way — or queued for a retry —
    // holds a later one back; it waits in the queue, untried, and goes right after the earlier one (Finish) or with
    // the next retry, in order.
    std::lock_guard lock(mutex_);
    if (WaitsBehindEarlierLocked(request.thread_id, request.message_id)) {
      log().info << "send waits behind an earlier message of its chat message_id=" << request.message_id;
      EnqueueLocked(Pending{.envelope = std::move(request.envelope),
                            .message_id = request.message_id,
                            .thread_id = request.thread_id,
                            .attempt_count = 0,
                            .amp_peer_key = request.amp_peer_key,
                            .group_member = request.group_member,
                            .copy_device = request.copy_device,
                            .relay_recipient = *request.relay_recipient,
                            .serial = serial,
                            .last_failure = Error("Waiting behind an earlier message")});
      return;
    }
  }
  auto work = [this, serial, request = std::move(request)]() mutable {
    if (EndIfCancelled(request.thread_id, request.message_id, serial)) {
      return;
    }
    // From the attempt's start until its outcome is written (the row marked, in the outcome handler): an app going to
    // the background meanwhile is given the time to hear the answer and record it (B236: a relay that took the message
    // while the app was being suspended left the row unsent for good).
    auto background = std::make_shared<BackgroundActivity>("message send");
    auto post_continue = [critical = request.critical_lane](std::function<void()> fn) {
      if (critical) {
        AppRuntime::PostWorkerCritical(std::move(fn));
      } else {
        AppRuntime::PostWorkerNormal(std::move(fn));
      }
    };
    // `direct`: the direct send's failure when it was tried — what the notice says when the relay
    // cannot carry the envelope either.
    auto finish_relay = [this, serial, request, background](std::optional<DirectSendFailure> direct) {
      // After a direct failure the relay is a new attempt.
      if (direct && EndIfCancelled(request.thread_id, request.message_id, serial)) {
        return;
      }
      DeliveryOutcome outcome{.thread_id = request.thread_id,
                              .message_id = request.message_id,
                              .group_member = request.group_member,
                              .copy_device = request.copy_device};
      if (!request.relay_recipient) {
        outcome.failure = direct ? SendFailureFromDirect(direct->GetCode()) : SendFailure::NoAddress;
        outcome.detail = direct ? direct->message : "Peer has no Account ID for the relay";
        Finish(std::move(outcome));
        return;
      }
      IRelayClient* relay = relay_.load(std::memory_order_acquire);
      if (!relay) {
        outcome.failure = direct ? SendFailureFromDirect(direct->GetCode()) : SendFailure::RelayNotConfigured;
        outcome.detail = direct ? direct->message : "Relay client not configured";
        Finish(std::move(outcome));
        return;
      }
      if (auto sent = relay->Send(request.envelope); !sent) {
        if (RelayRecipientNotRegistered(sent.error()) && TryAmpFallback(request, serial, sent.error(), background)) {
          return; // not on our relay: over a link to it, if one can be had
        }
        Pending pending{.envelope = request.envelope,
                        .message_id = request.message_id,
                        .thread_id = request.thread_id,
                        .attempt_count = 1,
                        .amp_peer_key = request.amp_peer_key,
                        .group_member = request.group_member,
                        .copy_device = request.copy_device,
                        .relay_recipient = *request.relay_recipient,
                        .serial = serial,
                        .last_failure = sent.error()};
        if (RelayStrangerQuota(sent.error())) {
          pending.not_before_ms = util::NowUnixMs() + StrangerQuotaRetryAfter(1).count();
        }
        const bool control_lane = request.thread_id.empty();
        bool again = false;
        if (control_lane) {
          again = RetryControlLater(std::move(pending), sent.error());
        } else if ((again = OutboxRetryAgain(1, sent.error())) && !Enqueue(std::move(pending))) {
          return; // Cancelled while it was on the way: it ends here.
        }
        log().warning << "relay send failed message_id=" << request.message_id << " peer=" << *request.relay_recipient
                      << " err=" << sent.error().message
                      << (!again         ? " (not retried)"
                          : control_lane ? " (retrying shortly)"
                                         : " (queued for retry)");
        outcome = RelayFailureOutcome(std::move(outcome), *request.relay_recipient, sent.error());
        // Queued for a retry: the send goes on, so the failure is reported without ending it (its row stays queued).
        if (again) {
          outcome.retrying = true;
          Report(std::move(outcome));
        } else {
          Finish(std::move(outcome));
        }
        return;
      }
      outcome.ok = true;
      outcome.transport = MessageTransport::Relay;
      outcome.relay_recipient = *request.relay_recipient;
      Finish(std::move(outcome));
    };

    IDirectMessageClient* chat = direct_chat_ ? direct_chat_() : nullptr;
    const bool reachable = chat && !request.amp_peer_key.empty() && chat->IsPeerReachable(request.amp_peer_key);
    const bool connected = chat && !request.amp_peer_key.empty() && chat->IsPeerConnected(request.amp_peer_key);
    if (ControlAlsoGoesDirect(connected, request.relay_recipient.has_value(), request.critical_lane)) {
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
    // Call signaling: Amp's open + ack per message races media setup — the relay first when it can
    // carry it; Amp remains for Amp-only peers.
    if (!DirectSendTriesAmpFirst(reachable, request.relay_recipient.has_value(), request.critical_lane)) {
      finish_relay(std::nullopt);
      return;
    }
    // Fire and forget: this worker is free while the mesh pump drives the open + ack.
    SendDirect(
        chat, request.amp_peer_key, request.envelope,
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

void DeliveryOutbox::SendDirect(IDirectMessageClient* chat, const std::string& peer_key, const RelayEnvelope& envelope,
                                std::function<void(DirectSendRoe)> on_done) {
  // B236 R4: whatever the transport does, the attempt ends — its answer, or after direct_send_timeout_ a retryable
  // "not confirmed" (the relay next, or the queue). Whichever comes first is the one; the other is dropped. A
  // transport that drops the callback unanswered leaves the timer's copy to end it.
  struct Once {
    std::atomic<bool> done{false};
    std::function<void(DirectSendRoe)> on_done;
  };
  auto once = std::make_shared<Once>();
  once->on_done = std::move(on_done);
  const uint64_t timer =
      AppRuntime::ScheduleCoordinatorOneShot(direct_send_timeout_, lifetime_.Bind([this, once, peer_key]() {
        if (once->done.exchange(true)) {
          return;
        }
        log().info << "direct send unanswered peer=" << peer_key << " after "
                   << std::chrono::duration_cast<std::chrono::seconds>(direct_send_timeout_).count() << " s";
        auto finish = std::move(once->on_done);
        finish(DirectSendFailure::Of(DirectSendErr::NotConfirmed, "Direct send unanswered"));
      }));
  chat->SendEnvelopeAsync(peer_key, envelope, [once, timer](DirectSendRoe direct) {
    if (once->done.exchange(true)) {
      return;
    }
    AppRuntime::CancelCoordinatorTimer(timer);
    auto finish = std::move(once->on_done);
    finish(std::move(direct));
  });
}

void DeliveryOutbox::EndBatch(const std::vector<std::string>& lanes, std::vector<Pending> still_pending) {
  std::vector<std::string> follow; // threads with untried items left: one more pass, in order
  {
    std::lock_guard lock(mutex_);
    for (const std::string& lane : lanes) {
      batch_lanes_.erase(lane);
    }
    for (Pending& item : still_pending) {
      EnqueueLocked(std::move(item));
    }
    for (const Pending& item : queue_) {
      if (item.attempt_count == 0 && item.not_before_ms == 0 &&
          std::find(lanes.begin(), lanes.end(), OrderLane(item)) != lanes.end() &&
          std::find(follow.begin(), follow.end(), item.thread_id) == follow.end()) {
        // Waited behind this batch's lane and was never tried: unless its lane is blocked by a failure the batch
        // queued again (that one goes first, at the next retry), it goes now.
        const bool behind_failure = std::any_of(queue_.begin(), queue_.end(), [&](const Pending& other) {
          return OrderLane(other) == OrderLane(item) && other.message_id != item.message_id &&
                 other.serial < item.serial && other.attempt_count > 0;
        });
        if (!behind_failure) {
          follow.push_back(item.thread_id);
        }
      }
    }
  }
  for (const std::string& thread_id : follow) {
    AppRuntime::PostWorkerNormal(lifetime_.Bind([this, thread_id]() { Retry(thread_id); }));
  }
}

bool DeliveryOutbox::TryAmpFallback(const DeliveryRequest& request, const uint64_t serial, const Error& relay_failure,
                                    std::shared_ptr<BackgroundActivity> background) {
  IDirectMessageClient* chat = direct_chat_ ? direct_chat_() : nullptr;
  if (request.amp_fallback_key.empty() || !chat || !chat->IsPeerReachable(request.amp_fallback_key)) {
    return false;
  }
  log().info << "relay does not know the recipient message_id=" << request.message_id << "; over a link to "
             << request.amp_fallback_key;
  SendDirect(
      chat, request.amp_fallback_key, request.envelope,
      [this, request, serial, relay_failure, background = std::move(background)](DirectSendRoe direct) {
        AppRuntime::PostWorkerNormal([this, request, serial, relay_failure, background, direct = std::move(direct)]() {
          if (EndIfCancelled(request.thread_id, request.message_id, serial)) {
            return;
          }
          DeliveryOutcome outcome{.thread_id = request.thread_id,
                                  .message_id = request.message_id,
                                  .group_member = request.group_member,
                                  .copy_device = request.copy_device};
          if (direct) {
            outcome.ok = true;
            outcome.transport = MessageTransport::Direct;
            Finish(std::move(outcome));
            return;
          }
          Finish(RelayFailureOutcome(std::move(outcome), request.relay_recipient.value_or(""), relay_failure));
        });
      });
  return true;
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
  const bool quota_held = RelayStrangerQuota(failure);
  const uint64_t timer = AppRuntime::ScheduleCoordinatorOneShot(*delay, lifetime_.Bind([this, seq]() {
    std::optional<Pending> due;
    {
      std::lock_guard lock(mutex_);
      auto it = control_retries_.find(seq);
      if (it == control_retries_.end()) {
        return; // Cancelled, or released by a grant, as it fired.
      }
      due = std::move(it->second.item);
      control_retries_.erase(it);
    }
    AppRuntime::PostWorkerCritical(
        lifetime_.Bind([this, item = std::move(*due)]() mutable { RetryControlNow(std::move(item)); }));
  }));
  // Recorded after scheduling (the timer's lock is not taken under ours); the delay is a second at least.
  std::lock_guard lock(mutex_);
  control_retries_[seq] =
      ControlRetry{.timer_id = timer, .message_id = message_id, .item = std::move(item), .quota_held = quota_held};
  return true;
}

void DeliveryOutbox::RetryControlNow(Pending item) {
  IRelayClient* relay = relay_.load(std::memory_order_acquire);
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
  const RelayEnvelope envelope = item.envelope;
  if (RetryControlLater(std::move(item), sent.error())) {
    return;
  }
  log().warning << "control retry gave up message_id=" << outcome.message_id << " attempts=" << attempts
                << " err=" << sent.error().message;
  Report(RelayFailureOutcome(std::move(outcome), envelope.recipient_account_id.value_or(""), sent.error()));
}

void DeliveryOutbox::Cancel(const std::string& message_id) {
  std::vector<uint64_t> timers;
  std::vector<std::string> wake; // chats whose later messages waited behind this one, untried (B236)
  {
    std::lock_guard lock(mutex_);
    std::vector<std::string> threads;
    std::erase_if(queue_, [&](const Pending& pending) {
      if (pending.message_id != message_id) {
        return false;
      }
      if (!pending.thread_id.empty()) {
        EndActiveLocked(pending.message_id);
        if (std::find(threads.begin(), threads.end(), pending.thread_id) == threads.end()) {
          threads.push_back(pending.thread_id);
        }
      }
      return true;
    });
    for (const std::string& thread_id : threads) {
      if (FollowersFreeLocked(thread_id)) {
        wake.push_back(thread_id);
      }
    }
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
  WakeFollowers(wake);
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

void DeliveryOutbox::ReleaseHeldFor(const std::string& recipient) {
  if (recipient.empty()) {
    return;
  }
  bool held = false;
  std::vector<std::pair<uint64_t, ControlRetry>> released; // by refusal order (seq)
  {
    std::lock_guard lock(mutex_);
    std::erase_if(control_retries_, [&](auto& entry) {
      if (!entry.second.quota_held || entry.second.item.relay_recipient != recipient) {
        return false;
      }
      released.emplace_back(entry.first, std::move(entry.second));
      return true;
    });
    for (Pending& item : queue_) {
      if (item.not_before_ms != 0 && item.relay_recipient == recipient) {
        item.not_before_ms = 0;
        held = true;
      }
    }
  }
  std::sort(released.begin(), released.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (auto& [seq, retry] : released) {
    AppRuntime::CancelCoordinatorTimer(retry.timer_id);
    RetryControlNow(std::move(retry.item));
  }
  if (held || !released.empty()) {
    log().info << "grant from " << recipient << " arrived: sending what its stranger quota held back (control "
               << released.size() << ")";
  }
  if (held) {
    Retry();
  }
}

void DeliveryOutbox::Retry(const std::string& only_thread) {
  std::vector<Pending> pending;
  std::vector<std::string> lanes; // the order lanes this batch takes (no other batch runs them meanwhile)
  const int64_t now_ms = util::NowUnixMs();
  {
    std::lock_guard lock(mutex_);
    std::vector<Pending> all;
    all.swap(queue_);
    // In the order they were first sent (B236): a lane's (a 1:1 chat's, a group member's) items go one after another,
    // and one not due yet (a stranger-quota backoff) holds back every later one of its lane.
    std::stable_sort(all.begin(), all.end(), [](const Pending& a, const Pending& b) { return a.serial < b.serial; });
    std::unordered_map<std::string, std::string> held; // lane -> the message holding it (its other copies may go)
    for (Pending& item : all) {
      const std::string lane = OrderLane(item);
      const bool other_thread = !only_thread.empty() && item.thread_id != only_thread;
      const bool lane_busy = batch_lanes_.contains(lane); // another batch is sending it: it keeps the order
      const auto holder = held.find(lane);
      const bool held_back = holder != held.end() && holder->second != item.message_id;
      if (other_thread || lane_busy || held_back || item.not_before_ms > now_ms) {
        if (!other_thread && holder == held.end()) {
          held.emplace(lane, item.message_id);
        }
        queue_.push_back(std::move(item));
        continue;
      }
      if (std::find(lanes.begin(), lanes.end(), lane) == lanes.end()) {
        lanes.push_back(lane);
      }
      pending.push_back(std::move(item));
    }
    for (const std::string& lane : lanes) {
      batch_lanes_.insert(lane);
    }
  }
  std::vector<Pending> dropped; // rows whose request ends here (not sent again)
  std::erase_if(pending, [this, now_ms, &dropped](const Pending& item) {
    const bool control_lane = item.thread_id.empty();
    if (OutboxSendStillWanted(control_lane, now_ms - item.envelope.timestamp, Paused(item.thread_id))) {
      return false;
    }
    if (!control_lane) {
      dropped.push_back(item);
    }
    return true;
  });
  for (const Pending& item : dropped) {
    Finish(DroppedOutcome(item)); // its row was queued until now: the last failure is its end (B236)
  }
  if (pending.empty()) {
    EndBatch(lanes, {});
    return;
  }
  AppRuntime::PostWorkerNormal([this, lanes, pending = std::move(pending)]() mutable {
    // As a first send's: the batch's outcomes are written before the app may be suspended (B236).
    auto background = std::make_shared<BackgroundActivity>("message retry");
    // No relay set (yet): each item still counts the try (try_relay: "Relay client not configured"), so one never
    // waits forever on a relay that does not come — it fails once its retries are used up (B236 R4).
    auto still_pending = std::make_shared<std::vector<Pending>>();
    // A lane whose item failed and is tried again later: its later items wait, untried, behind it (B236).
    auto blocked = std::make_shared<std::unordered_map<std::string, std::string>>(); // lane -> the failed message
    // Held by this call and by each send still in flight; it reaches itself only weakly. A strong
    // self-capture was a cycle that kept every batch's closure alive after it ended.
    auto process = std::make_shared<std::function<void(size_t)>>();
    *process = [this, lanes, pending = std::move(pending), still_pending, blocked, background,
                weak_process = std::weak_ptr<std::function<void(size_t)>>(process)](size_t index) mutable {
      const auto process = weak_process.lock();
      if (!process) {
        return;
      }
      if (index >= pending.size()) {
        EndBatch(lanes, std::move(*still_pending));
        return;
      }
      Pending item = std::move(pending[index]);
      if (const auto holder = blocked->find(OrderLane(item));
          holder != blocked->end() && holder->second != item.message_id) {
        still_pending->push_back(std::move(item)); // behind an earlier one still to be tried again: untried
        (*process)(index + 1);
        return;
      }
      if (Paused(item.thread_id)) {
        Finish(DroppedOutcome(item)); // dropped, not queued again: its request ends with its last failure
        (*process)(index + 1);
        return;
      }
      if (EndIfCancelled(item.thread_id, item.message_id, item.serial)) {
        (*process)(index + 1);
        return;
      }
      auto try_relay = [this, still_pending, blocked, process, index](Pending item) mutable {
        if (EndIfCancelled(item.thread_id, item.message_id, item.serial)) {
          (*process)(index + 1);
          return;
        }
        IRelayClient* relay = relay_.load(std::memory_order_acquire);
        auto result = relay ? relay->Send(item.envelope) : Roe<void>(Error("Relay client not configured"));
        if (result) {
          Finish(DeliveryOutcome{.thread_id = item.thread_id,
                                 .message_id = item.message_id,
                                 .ok = true,
                                 .transport = MessageTransport::Relay,
                                 .group_member = item.group_member,
                                 .copy_device = item.copy_device,
                                 .relay_recipient = item.envelope.recipient_account_id.value_or("")});
        } else if (OutboxRetryAgain(item.attempt_count, result.error())) {
          item.attempt_count += 1;
          item.last_failure = result.error();
          blocked->emplace(OrderLane(item), item.message_id);
          if (RelayStrangerQuota(result.error())) {
            item.not_before_ms = util::NowUnixMs() + StrangerQuotaRetryAfter(item.attempt_count).count();
          }
          still_pending->push_back(std::move(item));
        } else {
          Finish(RelayFailureOutcome(DeliveryOutcome{.thread_id = item.thread_id,
                                                     .message_id = item.message_id,
                                                     .group_member = item.group_member,
                                                     .copy_device = item.copy_device},
                                     item.envelope.recipient_account_id.value_or(""), result.error()));
        }
        (*process)(index + 1);
      };
      IDirectMessageClient* chat = direct_chat_ ? direct_chat_() : nullptr;
      const std::string peer_key =
          !item.amp_peer_key.empty() ? item.amp_peer_key : item.envelope.recipient_account_id.value_or("");
      if (!peer_key.empty() && chat && chat->IsPeerReachable(peer_key)) {
        const RelayEnvelope envelope = item.envelope;
        SendDirect(
            chat, peer_key, envelope,
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
