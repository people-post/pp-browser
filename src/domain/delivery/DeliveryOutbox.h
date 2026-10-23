#pragma once

#include "common/Module.h"
#include "common/chat/IDirectMessageClient.h"
#include "common/chat/RelayEnvelope.h"
#include "common/thread/ThreadTypes.h"
#include "lib/messaging/DeliveryNotice.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
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
  /** Call signaling: the critical worker lane, and the relay first when a route is known. */
  bool critical_lane = false;
};

/** How a request (or its retry) ended. Reported on a worker thread, in send order per request. */
struct DeliveryOutcome {
  std::string thread_id;
  std::string message_id;
  bool ok = false;
  MessageTransport transport = MessageTransport::Relay;
  /** Sent by the relay after the direct send was tried and failed. */
  bool relay_after_direct = false;
  SendFailure failure = SendFailure::NoAddress;
  std::string detail;
};

/**
 * Gets envelopes to peers: Amp first when the peer is reachable that way (DirectSendTriesAmpFirst:
 * the critical lane prefers a known relay route), else — or after a direct failure — the Brief relay;
 * a relay failure is queued and retried (OutboxRetryAgain; control-lane sends only while fresh,
 * OutboxSendStillWanted). Sends run on the worker pool, never on the caller's thread.
 *
 * Thread-safe. The outcome handler runs on a worker: the owner updates its rows there, before it
 * tells the UI, so a retract or cancel cannot race the mark.
 */
class DeliveryOutbox : public Module {
public:
  /** `direct_chat`: the attached direct-chat transport now (null: none — relay only). */
  explicit DeliveryOutbox(std::function<IDirectMessageClient*()> direct_chat);

  void SetRelayClient(IRelayClient* relay) { relay_ = relay; }
  void SetOnOutcome(std::function<void(const DeliveryOutcome&)> handler) { on_outcome_ = std::move(handler); }
  /** A thread whose sends are paused (e.g. its E2E session is compromised): its retries are dropped. */
  void SetThreadPaused(std::function<bool(const std::string& thread_id)> paused) { paused_ = std::move(paused); }

  void Send(DeliveryRequest request);
  /** Send what is queued again (when the inbox is synced: the relay is likely reachable). */
  void Retry();
  void Cancel(const std::string& message_id);
  /** Drop a thread's queued sends (synchronously: before its next send). */
  void PurgeThread(const std::string& thread_id);

private:
  struct Pending {
    RelayEnvelope envelope;
    std::string message_id;
    std::string thread_id;
    int attempt_count = 0;
  };

  void Report(DeliveryOutcome outcome);
  void Enqueue(Pending pending);
  bool Paused(const std::string& thread_id) const { return !thread_id.empty() && paused_ && paused_(thread_id); }

  std::function<IDirectMessageClient*()> direct_chat_;
  IRelayClient* relay_ = nullptr;
  std::function<void(const DeliveryOutcome&)> on_outcome_;
  std::function<bool(const std::string&)> paused_;
  mutable std::mutex mutex_;
  std::vector<Pending> queue_;
};

} // namespace pbr
