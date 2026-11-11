#pragma once

#include "lib/delivery/RelayClients.h"
#include "common/Lifetime.h"
#include "common/Module.h"
#include "common/chat/IDirectMessageClient.h"
#include "common/chat/RelayEnvelope.h"
#include "common/thread/ThreadTypes.h"
#include "lib/delivery/DeliveryNotice.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class IRelayClient;

/** One envelope to get to a peer. */
struct DeliveryRequest {
  RelayEnvelope envelope;
  /** The transcript row it carries; empty for the control lane (nothing to mark). */
  std::string thread_id;
  std::string message_id;
  /** Amp dial key of the peer (its account); empty: relay only. */
  std::string amp_peer_key;
  /** The peer's Brief `relay:` route; none: Amp only. */
  std::optional<std::string> relay_route;
  /** Time-critical signaling (a call's): the critical worker lane, and the relay first when a route is known. */
  bool critical_lane = false;
  /** One member's copy of a group message (their account): its outcome is that member's, not the row's. */
  std::string group_member;
  /**
   * One of a message's several records (M022: a device's copy over its link, the relay record for the rest):
   * the row is delivered once any is, and failed only while none is. Names the device, or "relay".
   */
  std::string copy_device;
};

/** How a request (or its retry) ended. Reported on a worker thread, in send order per request. */
struct DeliveryOutcome {
  std::string thread_id;
  std::string message_id;
  bool ok = false;
  MessageTransport transport = MessageTransport::Relay;
  SendFailure failure = SendFailure::NoAddress;
  std::string detail;
  /** As the request's. */
  std::string group_member;
  std::string copy_device;
};

/**
 * Gets envelopes to peers: Amp first when the peer is reachable that way (DirectSendTriesAmpFirst:
 * the critical lane prefers a known relay route, and also sends over a live link at once —
 * ControlAlsoGoesDirect), else — or after a direct failure — the Brief relay. A failed relay send is
 * queued and retried when the inbox syncs (OutboxRetryAgain), except on the control lane: signaling is
 * live, so it is sent again on a short backoff while it is fresh (ControlRetryAfter). Sends run on the
 * worker pool, never on the caller's thread.
 *
 * Thread-safe. The outcome handler runs on a worker: the owner updates its rows there, before it
 * tells the UI, so a retract or cancel cannot race the mark.
 */
class DeliveryOutbox : public Module {
public:
  /** `direct_chat`: the attached direct-chat transport now (null: none — relay only). */
  explicit DeliveryOutbox(std::function<IDirectMessageClient*()> direct_chat);
  /** Cancels the control lane's pending retries and waits out one that is running. */
  ~DeliveryOutbox();

  void SetRelayClient(IRelayClient* relay) { relay_ = relay; }
  void SetOnOutcome(std::function<void(const DeliveryOutcome&)> handler) { on_outcome_ = std::move(handler); }
  /** A thread whose sends are paused (e.g. its E2E session is compromised): its retries are dropped. */
  void SetThreadPaused(std::function<bool(const std::string& thread_id)> paused) { paused_ = std::move(paused); }

  void Send(DeliveryRequest request);
  /** Send what is queued again (when the inbox is synced: the relay is likely reachable). */
  void Retry();
  /** Drop a message's queued sends and its control-lane retries. */
  void Cancel(const std::string& message_id);
  /** Drop a thread's queued sends (synchronously: before its next send). */
  void PurgeThread(const std::string& thread_id);
  /**
   * When the send of transcript row `message_id` that is still under way started: from its first request's Send
   * until its last request ends (delivered, failed and not queued again, dropped, cancelled), counting each of a
   * group's members and each copy. None when nothing of it is in flight or queued. In memory only (none after a
   * restart); the control lane is not tracked. It changes nothing that is reported or retried. Any thread.
   */
  std::optional<int64_t> SendStartedMs(const std::string& message_id) const;

private:
  struct Pending {
    RelayEnvelope envelope;
    std::string message_id;
    std::string thread_id;
    int attempt_count = 0;
    /** The request's Amp dial key (empty: the envelope's recipient) and group member. */
    std::string amp_peer_key;
    std::string group_member;
    std::string copy_device;
  };

  /** A control-lane retry waiting on its timer. */
  struct ControlRetry {
    uint64_t timer_id = 0;
    std::string message_id;
  };

  /** An outcome after which the request goes on (failed, queued for a retry). */
  void Report(DeliveryOutcome outcome);
  /** A request's last outcome: it leaves `active_` first, then is reported as Report does. */
  void Finish(DeliveryOutcome outcome);
  /** A transcript row's request starts (Send); the control lane (no thread) is not tracked. */
  void BeginActive(const std::string& thread_id, const std::string& message_id);
  /** One request of `message_id` ended. Holds `mutex_`. */
  void EndActiveLocked(const std::string& message_id);
  void Enqueue(Pending pending);
  /** Schedule the control-lane send again after `failure` (ControlRetryAfter); false: given up. */
  bool RetryControlLater(Pending item, const Error& failure);
  /** A control-lane retry's turn: over the relay, again later when that fails. */
  void RetryControlNow(Pending item);
  bool Paused(const std::string& thread_id) const { return !thread_id.empty() && paused_ && paused_(thread_id); }

  std::function<IDirectMessageClient*()> direct_chat_;
  IRelayClient* relay_ = nullptr;
  std::function<void(const DeliveryOutcome&)> on_outcome_;
  std::function<bool(const std::string&)> paused_;
  /** A row's sends under way: when the first started, and how many requests have not ended. */
  struct ActiveSend {
    int64_t started_ms = 0;
    int requests = 0;
  };

  mutable std::mutex mutex_;
  std::vector<Pending> queue_;
  /** By message id (SendStartedMs). */
  std::unordered_map<std::string, ActiveSend> active_;
  /** By a sequence number of ours: the timer's id is not known until it is scheduled. */
  std::unordered_map<uint64_t, ControlRetry> control_retries_;
  uint64_t next_control_retry_ = 0;
  /** What the retry timers run binds to it: nothing reaches a destroyed outbox. */
  pp::Lifetime lifetime_;
};

} // namespace pbr
