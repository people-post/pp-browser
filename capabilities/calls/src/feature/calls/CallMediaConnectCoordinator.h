#pragma once

#include "domain/call_media/ICallMediaTransport.h"
#include "feature/calls/CallConnectDiag.h"
#include "mesh/reach/PeerReachCoordinator.h"
#include "feature/calls/CallSessionEvents.h"
#include "common/runtime/OwnerOutbox.h"

#include "common/Module.h"

#include <atomic>
#include <cstdint>
#include <chrono>
#include <vector>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/** One connect sequence: the link to reach and the call-media bundle to open on it. */
struct CallMediaConnectRequest {
  /** Bundle params (call_id / epoch / key / glare role) — passed to the transport as-is. */
  CallMediaDirectConnectParams params;
  CallMediaDirectCallbacks callbacks;
  /** Link for the first attempt; later attempts drop exclude_direct and set fresh_link as needed. */
  mesh::PeerReachRequest reach;
};

struct CallMediaConnectHooks {
  /** After the link is ready, before each ConnectAsync (e.g. offerer resends the media key). */
  std::function<void(const CallMediaDirectConnectParams&)> before_attempt;
  /** Link settled for an attempt. */
  std::function<void(mesh::PeerLinkKind kind)> on_link_ready;
  /**
   * Sequence ended on its own: ok = bundle MediaReady, error = the last attempt's code (attempts
   * exhausted), or the peer reach's wrapped (`WrapReachFailure`). Runs as its own event; skipped
   * when Abort / Start ran after the sequence ended. Never runs for Abort.
   */
  std::function<void(CallMediaRoe result)> on_finished;
};

/**
 * Connect sequence for 1:1 call-media: per attempt, reach a link (PeerReachCoordinator) then open
 * the bundle on it (ICallMediaTransport::ConnectAsync), with a per-attempt watchdog (B42) and up
 * to kConnectAttempts retries. A failed attempt that ran on a reused or dead link asks the next
 * reach for a fresh link (B39). Knows the bundle protocol, not the call product (no engine, seat,
 * planner or SFU).
 *
 * Outbound only: inbound hellos are the device's doorway's (CallMediaDoorway, V056).
 *
 * Threading: passive, on the calls owner (THREADING.md § Owner runners). Its timers, reach / bundle
 * results come back through its outbox as `ConnectEvent`s, handled by `Handle` (the bridge routes
 * them). `InFlight()` is safe from any thread.
 */
class CallMediaConnectCoordinator : public Module {
public:
  CallMediaConnectCoordinator(ICallMediaTransport& transport, mesh::PeerReachCoordinator& reach);

  /** A peer reach that found no link, as this layer's code (CODED_FAILURE.md: wrap the child). */
  static CallMediaFailure WrapReachFailure(const mesh::PeerReachCoordinator::Failure& child);
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
  /** Abort and refuse later Starts. */
  void Shutdown();

  /** Where its events go (the bridge wraps them). */
  void SetOutbox(OwnerOutbox<ConnectEvent> outbox);
  /** One of its own events, back on the calls owner. */
  void Handle(ConnectEvent& event);

  bool InFlight() const { return inflight_.load(std::memory_order_acquire); }

  /** Per-attempt ConnectAsync timeout (<= 0 = production default). */
  void SetAttemptTimeoutMsForTest(int timeout_ms);

  /**
   * What the connect lines of the Diag channel read of this device and the mesh (calls owner; empty =
   * lines without it). Cleared when the mesh ports are detached.
   */
  void SetDiagProbe(CallConnectEnvProbe probe);

private:
  bool Current(uint64_t seq) const;
  bool MediaReady() const;
  void BeginAttempt(uint64_t seq, int attempt);
  void OnLinkReady(uint64_t seq, int attempt, mesh::PeerReachCoordinator::ReachRoe reached);
  void OpenBundle(uint64_t seq, int attempt);
  void ArmWatchdog(uint64_t seq, int attempt);
  void OnWatchdog(uint64_t seq, int attempt);
  void OnAttemptFinished(uint64_t seq, int attempt, CallMediaRoe connected);
  void Finish(uint64_t seq, CallMediaRoe result);
  void CancelTimers();
  const char* Role() const;
  ICallMediaTransport& transport_;
  mesh::PeerReachCoordinator& reach_;
  std::atomic<uint64_t> seq_{0};
  std::atomic<bool> inflight_{false};
  std::atomic<bool> shut_down_{false};
  OwnerOutbox<ConnectEvent> outbox_;

  // Calls owner.
  CallMediaConnectRequest request_;
  CallMediaConnectHooks hooks_;
  mesh::PeerReachId reach_id_ = 0;
  int attempt_current_ = 0;
  bool attempt_reused_link_ = false;
  bool fresh_link_next_ = false;
  OwnerExecutor::TimerId retry_timer_id_ = 0;
  OwnerExecutor::TimerId watchdog_timer_id_ = 0;
  int attempt_timeout_ms_;

  // Diag lines of the sequence (calls owner). Reset by Start.
  CallConnectEnvProbe diag_probe_;
  std::chrono::steady_clock::time_point diag_started_;
  int diag_attempts_ = 0;
  mesh::PeerLinkKind diag_kind_ = mesh::PeerLinkKind::Unknown;
  int diag_family_ = 0;
  bool diag_reach_failed_ = false;
  mesh::PeerReachCoordinator::Err diag_reach_err_ = mesh::PeerReachCoordinator::Err::Ok;
  ShareWord diag_why_ = "none";
  CallConnectEnv DiagEnv() const;
  int64_t DiagMs() const;
};

} // namespace pbr
