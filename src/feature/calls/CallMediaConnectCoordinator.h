#pragma once

#include "domain/mesh/l4/call_media/ICallMediaTransport.h"
#include "feature/calls/PeerReachCoordinator.h"

#include "common/Module.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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

/** An inbound call-media hello the transport is asking us to accept. */
struct CallMediaInboundHello {
  std::string call_id;
  uint32_t media_epoch = 1;
  /** Dialer's mesh PeerId (the link it arrived on). */
  std::string peer_id;
};

/**
 * What the owner decides about inbound bundles. All run on the transport's worker hop — they must
 * be thread-safe and must not touch UI-thread state directly (post instead).
 */
struct CallMediaInboundPorts {
  /** Session exists and still wants media; false → reject the hello. */
  std::function<bool(const std::string& call_id)> session_open;
  /** Epoch key for the hello, if already stored. */
  std::function<std::optional<ByteVector>(const std::string& call_id, uint32_t epoch)> load_key;
  /** Ask for the key to be fetched (e.g. inbox sync); called while waiting. */
  std::function<void(const std::string& call_id)> request_key;
  /** Bundle accepted (key found): return its callbacks. */
  std::function<CallMediaDirectCallbacks(const CallMediaInboundHello& hello)> on_accepted;
};

/**
 * Connect sequence for 1:1 call-media: per attempt, reach a link (PeerReachCoordinator) then open
 * the bundle on it (ICallMediaTransport::ConnectAsync), with a per-attempt watchdog (B42) and up
 * to kConnectAttempts retries. A failed attempt that ran on a reused or dead link asks the next
 * reach for a fresh link (B39). Knows the bundle protocol, not the call product (no engine, seat,
 * planner or SFU).
 *
 * Inbound: owns the transport's inbound handler. A hello is accepted once its epoch key is
 * available — the offerer often dials before the relay delivers the key, so the handler waits
 * (cancelable, bounded) on the worker hop, asking for the key meanwhile (V033: no bare sleep).
 *
 * Threading: API and sequence state on the UI thread; timers hop from the Coordinator to UI.
 * `InFlight()` and `NotifyKeyAvailable()` are safe from any thread.
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
  /** Abort, refuse later Starts, release waiting inbound hellos and drop the inbound handler. */
  void Shutdown();

  /** Install the inbound handler. Call once, before the transport can deliver hellos. */
  void SetInboundPorts(CallMediaInboundPorts ports);
  /** A media key landed — wake inbound hellos waiting for one. */
  void NotifyKeyAvailable();

  bool InFlight() const { return inflight_.load(std::memory_order_acquire); }

  /** Per-attempt ConnectAsync timeout (<= 0 = production default). */
  void SetAttemptTimeoutMsForTest(int timeout_ms);
  /** Inbound hello key wait budget (<= 0 = production default). */
  void SetInboundKeyWaitMsForTest(int wait_ms);

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
  void CheckUiThread(const char* what) const;
  void HandleInboundHello(CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs);
  /** Fills params.media_key if it arrives in time. False → reject (session gone / shut down). */
  bool WaitForInboundKey(CallMediaDirectConnectParams& params);

  ICallMediaTransport& transport_;
  PeerReachCoordinator& reach_;
  std::atomic<uint64_t> seq_{0};
  std::atomic<bool> inflight_{false};
  std::atomic<bool> shut_down_{false};
  std::shared_ptr<std::atomic<bool>> alive_;

  // Inbound (transport worker hop). Ports are set once before traffic.
  CallMediaInboundPorts inbound_ports_;
  bool inbound_installed_ = false;
  std::mutex inbound_mu_;
  std::condition_variable inbound_cv_;
  /** Bumped by NotifyKeyAvailable (under inbound_mu_) so a waiting hello re-checks at once. */
  uint64_t key_notices_ = 0;
  std::atomic<int> inbound_key_wait_ms_;

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
