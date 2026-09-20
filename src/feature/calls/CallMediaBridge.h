#pragma once

#include "domain/media/CallMediaEngine.h"
#include "domain/messaging/CallSessionStore.h"
#include "domain/messaging/CallMediaKeyStore.h"
#include "feature/calls/CallMediaHost.h"
#include "feature/calls/CallMediaSeat.h"
#include "domain/messaging/CallDirectPlannerLogic.h"
#include "feature/calls/CallTopologyRelayDeps.h"
#include "domain/mesh/l4/call_media/ICallMediaTransport.h"

#include "common/Module.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Direct arming / outcomes — CallMediaBridge consumer contract (V048).
 * Empty ports = permissive (unit tests).
 */
struct CallDirectArmingPorts {
  std::function<bool()> direct_ops_allowed;
  std::function<void(const std::string& call_id)> request_direct_arming;
  std::function<void(CallDirectPlannerPhase phase, const std::string& call_id)> report_progress;
  std::function<void(const std::string& call_id)> on_connected;
  std::function<void(const std::string& call_id)> on_connect_failed;
  std::function<void(const std::string& call_id)> on_media_deferred;
  std::function<void(const std::string& call_id)> on_media_key_ready;
  std::function<const char*()> arming_debug_name;

  bool IsBound() const { return static_cast<bool>(direct_ops_allowed); }
};

/**
 * MediaSeat façade for Direct path (V048 Bridge facet).
 * Bridge must not hold CallMediaSeat* — Stack projects.
 */
struct CallDirectSeatPorts {
  std::function<CallMediaSeat::Token(const std::string& call_id)> acquire;
  std::function<bool(const CallMediaSeat::Token& token)> allows_path_op;
  std::function<CallMediaSeat::Token()> current_token;
  std::function<std::string()> bound_call_id;
  std::function<void(const std::string& call_id)> note_connecting;
  std::function<void(const std::string& call_id)> note_start;
  std::function<void(CallMediaSeat::PathKind kind)> note_path;
  std::function<void(const std::string& call_id)> note_live;
  std::function<void(const std::string& call_id)> note_failed;

  bool IsBound() const { return static_cast<bool>(acquire); }
};

/**
 * 1:1 call media (m1 / V026) — V036 Phase 3 **Direct path** plugin under CallMediaSeat.
 * Uses CallMediaEngine SFU-mode capture/playback with Opus frames over ICallMediaTransport
 * (Amp; [A020]). Path Start / ReleaseTransport require a seat token when the seat is wired.
 */
class CallMediaBridge : public Module {
public:
  CallMediaBridge(CallMediaHost& host, CallSessionStore& sessions, CallMediaKeyStore& media_keys,
                        CallMediaEngine& media, ICallMediaTransport& direct, IDialRegistry* dial,
                        ICircuitHopReach* circuit_reach);

  bool IsMeshConnectFailed() const;
  bool MeshConnectMissingMic() const;
  void ClearMeshConnectFailed();
  void PollMeshConnectHealth();

  /** True when mesh call-media path is available (direct or circuit-brokered). */
  bool ShouldUseMeshForPeer(const std::string& peer_identity) const;

  Roe<void> RetryMeshMedia(const std::string& call_id);

  Roe<void> StartMediaAsOfferer(const std::string& call_id, const std::string& peer_identity);
  Roe<void> StartMediaAsAnswerer(const std::string& call_id, const std::string& peer_identity);
  void ScheduleStartMediaAsOfferer(const std::string& call_id, const std::string& peer_identity);
  void ScheduleStartMediaAsAnswerer(const std::string& call_id, const std::string& peer_identity);

  /** Answerer media waits for CallMediaKey (V015 epoch-1-on-accept); kick Start when key lands. */
  void OnMediaKeyReady(const std::string& call_id);

  /**
   * Test seam: answerer deferred-key inbox poll rounds (production default 90 ≈ 90s).
   * Set 0 so KeyTimeout → ConnectFailed is reachable without a long sleep.
   */
  void SetMediaKeyInboxPollRoundsForTest(int rounds);
  /** Shrink EnsurePeerReachable deadline for gtests (0 = production default). */
  void SetDialWaitBudgetMsForTest(int budget_ms);

  /**
   * SoftMigrate: close 1:1 call-media stream without CallMediaEngine::Stop so SFU capture continues.
   * Prefer ReleaseDirectTransport(token) when a MediaSeat is wired.
   */
  void ReleaseDirectTransport();
  /** V036 Phase 3: token-gated SoftMigrate release (no-op when token not bound). */
  void ReleaseDirectTransport(const CallMediaSeat::Token& token);

  /**
   * Engine Stop — **seat teardown hook only** when MediaSeat is wired (V036).
   * CallSessionManager Leave/Accept must use seat.Release, not this.
   */
  void StopMeshMedia(const std::string& call_id);
  /**
   * CallAccept/Invite taught PeerId→relay: (works for non-contacts). Rebind deferred inbound
   * on_audio stream_id when it matches the pending inbound PeerId.
   */
  void NotePeerIdRelayMapping(const std::string& peer_id, const std::string& relay_identity);

  /**
   * Abort in-flight Connect (generation bump + Detach). Prefer timeout_ms=0 on shutdown so
   * the UI/shutdown strand does not sleep-spin; late Connect callbacks no-op on generation.
   * Must run before destroying this bridge / CallMediaDirectService / mesh host.
   */
  void PrepareForTeardown(int timeout_ms = 0);

  /** True while an async Connect sequence is in flight (shutdown measurement). */
  bool IsConnectWorkerInflight() const {
    return connect_worker_inflight_.load(std::memory_order_acquire);
  }

  /** True after PrepareForTeardown / StopMeshMedia — inbound hello wait must exit. */
  bool IsStopping() const { return stopping_.load(std::memory_order_acquire); }

  void NoteMediaAttempted(const std::string& call_id);
  bool MediaAttempted(const std::string& call_id) const;

  /** Keep dial/circuit pointers valid when ConversationsHub rewires deps (N025 listen sync). */
  void SetReachDeps(IDialRegistry* dial, ICircuitHopReach* circuit_reach);
  /** Fire-and-forget bootstrap seed warm (CallStack::WarmBootstrapSeedSessions). */
  void SetSeedWarm(std::function<void()> warm);
  /** Answerer: park circuit reserve on org seed (CallStack::ReserveOnBootstrapSeeds). */
  void SetSeedReserve(std::function<void()> reserve);

  void SetDirectArmingPorts(CallDirectArmingPorts ports);
  /** V036 exclusive media epoch — Stack installs; Bridge must not hold CallMediaSeat*. */
  void SetSeatPorts(CallDirectSeatPorts ports);

  /** Last successful 1:1 reach mode: direct | punched | circuit (empty before connect). */
  std::string MediaPathKind() const;
  /** True when 1:1 call-media stream is up (not merely CallMediaEngine StartSfu). */
  bool HasActiveDirectStream() const;

  /** V039 Direct planner Apply — product callbacks on UI. */
  void Apply(CallDirectPlannerEvent ev, const std::string& call_id = {},
             const std::string& peer_identity = {});
  CallDirectPlannerPhase DirectPlannerPhase() const { return direct_planner_phase_; }

private:
  Roe<void> BeginSession(const std::string& call_id, const std::string& peer_identity, bool offerer);
  /** Circuit/punch reach without parking MeshControl (TryEnsureCallMediaReachableAsync). */
  void EnsurePeerReachableAsync(const std::string& peer_identity, uint64_t connect_gen,
                                std::function<void(Roe<void>)> on_done);
  /** Async dial/retry — does not park MeshControl for Connect timeout (ConnectAsync). */
  void StartConnectSequence(CallMediaDirectConnectParams params, CallMediaDirectCallbacks cbs, uint64_t gen);
  void ScheduleOffererGracePoll(CallMediaDirectConnectParams params, CallMediaDirectCallbacks cbs, uint64_t gen,
                                int64_t grace_deadline_ms);
  void BeginConnectAttempt(CallMediaDirectConnectParams params, CallMediaDirectCallbacks cbs, uint64_t gen,
                           int attempt);
  void ContinueConnectAttemptAfterReachable(CallMediaDirectConnectParams params, CallMediaDirectCallbacks cbs,
                                            uint64_t gen, int attempt);
  void OnConnectAttemptFinished(CallMediaDirectConnectParams params, CallMediaDirectCallbacks cbs, uint64_t gen,
                                int attempt, Roe<void> connected);
  void FinishConnectSequence(uint64_t gen, const std::string& call_id, Roe<void> connected, const char* role);
  /** Chrome ConnectFailed + Direct Idle; optionally StopMeshMedia (zombie TX / teardown). */
  void SurfaceConnectFailed(const std::string& call_id, const std::string& err, bool stop_media);
  void CancelConnectTimers();
  Roe<ByteVector> LoadActiveMediaKey(const std::string& call_id) const;
  /** Direct stream up: mark media connected when capture is live, always advance lifecycle/chrome. */
  void CommitDirectConnected(const std::string& call_id);
  void DeliverInboundDirectMedia(const std::string& call_id, uint8_t channel,
                                 const std::vector<uint8_t>& payload);
  void ReleaseDirectTransportBody();
  /** NAT dogfood: dialable "direct" with TX-only → force circuit ensure + re-dial. */
  void MaybeEscalateTxOnlyDirect();
  void EscalateTxOnlyViaCircuit(const std::string& call_id, const std::string& peer);
  void SetDirectPlannerPhase(CallDirectPlannerPhase next, CallDirectPlannerEvent ev,
                             const std::string& call_id);
  CallDirectPlannerApplyContext BuildDirectPlannerContext(const std::string& call_id,
                                                          const std::string& peer_identity) const;
  /** Arm health / TX-only / connect-timeout timer (pm3). */
  void ArmDirectHealthTimer();
  void CancelDirectHealthTimer();
  void OnDirectHealthTimerFire();

  CallMediaHost& host_;
  CallSessionStore& sessions_;
  CallMediaKeyStore& media_keys_;
  CallMediaEngine& media_;
  ICallMediaTransport& direct_;
  IDialRegistry* dial_ = nullptr;
  ICircuitHopReach* circuit_reach_ = nullptr;
  CallDirectArmingPorts arming_;
  CallDirectSeatPorts seat_;
  std::function<void()> seed_warm_;
  std::function<void()> seed_reserve_;
  /** direct | punched | circuit — set by EnsurePeerReachableAsync. */
  std::string media_path_kind_;
  /** When true, EnsurePeerReachableAsync must try circuit even if already dialable. */
  bool force_circuit_ensure_ = false;
  bool session_offerer_ = false;
  int64_t direct_connected_at_ms_ = 0;
  bool tx_only_escalation_done_ = false;

  std::string media_peer_identity_;
  std::string media_call_id_;
  std::string pending_answerer_call_id_;
  std::string pending_answerer_peer_;
  /** Inbound hello PeerId while stream_id deferred (non-contact / pre-Accept). */
  std::string inbound_deferred_peer_id_;
  bool mesh_connect_failed_ = false;
  bool mesh_connect_missing_mic_ = false;
  /** Connect sequence in flight (async ConnectAsync / grace poll / reachability). */
  std::atomic<bool> connect_worker_inflight_{false};
  /** Bumped in StopMeshMedia so in-flight Connect workers abort instead of racing Detach/Stop. */
  std::atomic<uint64_t> connect_generation_{0};
  std::atomic<bool> stopping_{false};
  /** Cancelable inbound hello MediaKey wait (notify from OnMediaKeyReady / PrepareForTeardown). */
  std::mutex inbound_key_mu_;
  std::condition_variable inbound_key_cv_;
  uint64_t offerer_grace_timer_id_ = 0;
  uint64_t connect_retry_timer_id_ = 0;
  uint64_t direct_health_timer_id_ = 0;
  CallDirectPlannerPhase direct_planner_phase_ = CallDirectPlannerPhase::Idle;
  std::unordered_set<std::string> media_attempted_calls_;
  int media_key_inbox_poll_rounds_ = 90;
  int64_t dial_wait_budget_ms_ = 12000;
  std::atomic<uint32_t> audio_seq_{0};
  /** 1:1 inbound remote mixer stream; 0 = defer until relay: identity known (BeginSession). */
  std::atomic<uint32_t> inbound_remote_stream_{0};
};

} // namespace pbr
