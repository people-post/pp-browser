#pragma once

#include "domain/mesh/l4/call_media/ICallMediaTransport.h"
#include "feature/calls/PeerReachCoordinator.h"

#include "common/Module.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include "common/PbrCompat.h"

namespace pbr {

/** One connect sequence: the link to reach and the call-media bundle to open on it. */
struct CallMediaConnectRequest {
  /** Bundle params (call_id / epoch / key / glare role) — passed to the transport as-is. */
  CallMediaDirectConnectParams params;
  CallMediaDirectCallbacks callbacks;
  /** Link for the first attempt; later attempts drop exclude_direct and set fresh_link as needed. */
  PeerReachRequest reach;
};

struct CallMediaConnectHooks {
  /** After the link is ready, before each ConnectAsync (e.g. offerer resends the media key). */
  std::function<void(const CallMediaDirectConnectParams&)> before_attempt;
  /** Link settled for an attempt. */
  std::function<void(PeerLinkKind kind)> on_link_ready;
  /**
   * Sequence ended on its own: ok = bundle MediaReady, error = attempts exhausted / unreachable.
   * Posted to UI; skipped when Abort / Start ran after the sequence ended. Never runs for Abort.
   */
  std::function<void(Roe<void> result)> on_finished;
};

/**
 * Connect sequence for 1:1 call-media: per attempt, reach a link (PeerReachCoordinator) then open
 * the bundle on it (ICallMediaTransport::ConnectAsync), with a per-attempt watchdog (B42) and up
 * to kConnectAttempts retries. A failed attempt that ran on a reused or dead link asks the next
 * reach for a fresh link (B39). Knows the bundle protocol, not the call product (no engine, seat,
 * planner or SFU).
 *
 * Threading: API and sequence state on the UI thread; timers hop from the Coordinator to UI.
 * `InFlight()` is safe from any thread.
 */
class CallMediaConnectCoordinator : public Module {
public:
  CallMediaConnectCoordinator(ICallMediaTransport& transport, PeerReachCoordinator& reach);
  ~CallMediaConnectCoordinator() override;

  CallMediaConnectCoordinator(const CallMediaConnectCoordinator&) = delete;
  CallMediaConnectCoordinator& operator=(const CallMediaConnectCoordinator&) = delete;

  /** Aborts any sequence in flight, then starts a new one. After Shutdown: fails the callbacks. */
  void Start(CallMediaConnectRequest request, CallMediaConnectHooks hooks);
  /**
   * Invalidates the sequence, cancels timers and the pending reach, clears InFlight. Completes
   * nothing (the caller initiated it — THREADING.md Cancel / Abort contract).
   */
  void Abort();
  /** Abort and refuse later Starts (teardown). */
  void Shutdown();

  bool InFlight() const { return inflight_.load(std::memory_order_acquire); }

  /** Per-attempt ConnectAsync timeout (<= 0 = production default). */
  void SetAttemptTimeoutMsForTest(int timeout_ms);

private:
  bool Current(uint64_t seq) const;
  bool MediaReady() const;
  void BeginAttempt(uint64_t seq, int attempt);
  void OnLinkReady(uint64_t seq, int attempt, Roe<PeerReachResult> reached);
  void OpenBundle(uint64_t seq, int attempt);
  void ArmWatchdog(uint64_t seq, int attempt);
  void OnAttemptFinished(uint64_t seq, int attempt, Roe<void> connected);
  void Finish(uint64_t seq, Roe<void> result);
  void CancelTimers();
  const char* Role() const;

  ICallMediaTransport& transport_;
  PeerReachCoordinator& reach_;
  std::atomic<uint64_t> seq_{0};
  std::atomic<bool> inflight_{false};
  std::atomic<bool> shut_down_{false};
  std::shared_ptr<std::atomic<bool>> alive_;

  // UI thread.
  CallMediaConnectRequest request_;
  CallMediaConnectHooks hooks_;
  PeerReachId reach_id_ = 0;
  int attempt_current_ = 0;
  bool attempt_reused_link_ = false;
  bool fresh_link_next_ = false;
  uint64_t retry_timer_id_ = 0;
  uint64_t watchdog_timer_id_ = 0;
  int attempt_timeout_ms_;
};

} // namespace pbr
