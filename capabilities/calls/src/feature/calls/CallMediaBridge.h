#pragma once

#include "lib/calls/CallPathPolicy.h"
#include "lib/media/MediaEngine.h"
#include "lib/calls/ICallSessionStore.h"
#include "lib/calls/ICallMediaKeyStore.h"
#include "feature/calls/CallMediaHost.h"
#include "feature/calls/CallDirectDriver.h"
#include "lib/calls/CallMediaSeat.h"
#include "feature/calls/CallSessionEvents.h"
#include "common/runtime/OwnerOutbox.h"
#include "lib/calls/CallDirectPlannerLogic.h"
#include "feature/calls/CallTopologyRelayDeps.h"
#include "feature/calls/CallMediaConnectCoordinator.h"
#include "feature/calls/CallMediaDoorway.h"
#include "mesh/reach/PeerReachCoordinator.h"
#include "domain/call_media/ICallMediaTransport.h"

#include "common/Module.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * What the 1:1 path asks of the calls it serves: may it run now (the call's Status arms one planner). Its
 * outcomes go up as reports (DirectReport). Empty ports = permissive (unit tests).
 */
struct CallDirectArmingPorts {
  std::function<bool()> direct_ops_allowed;
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
  std::function<void(const std::string& call_id)> note_start;

  bool IsBound() const { return static_cast<bool>(acquire); }
};

/**
 * 1:1 call media (m1 / V026) — V036 Phase 3 **Direct path** plugin under CallMediaSeat.
 * Uses MediaEngine SFU-mode capture/playback with Opus frames over ICallMediaTransport
 * (Amp; [A020]). Path Start / ReleaseTransport require a seat token when the seat is wired.
 */
class CallMediaBridge : public Module, public CallDirectDriver {
public:
  /** The 1:1 path of `call_id` (V056): built for that call by its `CallMedia`, lives with it. */
  CallMediaBridge(std::string call_id, CallMediaHost& host, ICallSessionStore& sessions, ICallMediaKeyStore& media_keys,
                  MediaEngine& media, ICallMediaTransport& direct, mesh::PeerReachPorts reach);
  const std::string& CallId() const { return call_id_; }
  /** The call's "the hop carries its media" flag: once set, 1:1 frames are dropped (before any traffic). */
  void SetHopCarriesMediaFlag(std::shared_ptr<const std::atomic<bool>> flag) {
    receive_->hop_carries_media = std::move(flag);
  }
  ~CallMediaBridge();

  bool IsMeshConnectFailed() const;
  bool MeshConnectMissingMic() const;
  /** The last bootstrap seed park for this connect timed out: no seed reachable (VPN / firewall). */
  bool MeshConnectSeedUnreachable() const;
  void ClearMeshConnectFailed();
  void PollMeshConnectHealth();

  Roe<void> RetryMeshMedia(const std::string& call_id) override;
  /**
   * The call failed but is still open, and the peer's connection for it is up (its Retry): restart
   * the engine as this side's role and commit over that stream (no Detach, no redial).
   */
  Roe<void> ResumeMeshMediaFromInbound(const std::string& call_id) override;
  /**
   * Retry / resume restart a call whose planner went Idle on failure: arm it again (Schedule →
   * Arming, key already held → Connecting) so the restarted connect's ConnectSucceeded lands.
   */
  void ArmPlannerForRestart(const std::string& call_id, const std::string& peer_identity, bool offerer);

  void ScheduleStartMediaAsOfferer(const std::string& call_id, const std::string& peer_identity);
  void ScheduleStartMediaAsAnswerer(const std::string& call_id, const std::string& peer_identity);

  /** Answerer media waits for CallMediaKey (V015 epoch-1-on-accept); kick Start when key lands. */
  void OnMediaKeyReady(const std::string& call_id) override;
  /** The doorway accepted an inbound bundle for `hello.call_id`: bind its peer, return its callbacks (calls owner). */
  CallMediaDirectCallbacks AcceptInbound(const CallMediaInboundHello& hello) override;

  /**
   * Test seam: answerer deferred-key inbox poll rounds (production default 90 ≈ 90s).
   * Set 0 so KeyTimeout → ConnectFailed is reachable without a long sleep.
   */
  void SetMediaKeyInboxPollRoundsForTest(int rounds);
  /** Shrink the peer-reach direct-dial budget for gtests (0 = production default). */
  void SetDialWaitBudgetMsForTest(int budget_ms);
  /** Tests: the answerer's wait for the caller's circuit (kCallAnswererAwaitHoldMs). */
  void SetAnswererAwaitHoldMsForTest(int64_t hold_ms) { answerer_await_hold_ms_ = hold_ms; }
  void SetReserveRenewIntervalMsForTest(int interval_ms) { reserve_renew_interval_ms_ = interval_ms; }
  /** Retry delay between re-anchor attempts while reconnecting (production 2 s). */
  void SetReanchorRetryMsForTest(int delay_ms) { reanchor_retry_ms_ = delay_ms; }
  /** Test: the TX-only grace expired now (skips the 15 s / 80-frame gate). */
  void EscalateTxOnlyForTest(const std::string& call_id) {
    tx_only_escalation_done_ = true;
    Apply(CallDirectPlannerEvent::TxOnlyGraceExpired, call_id, media_peer_identity_);
  }
  /**
   * k5: the device's network changed (calls owner). Links are being re-validated — Amp evicts the
   * dead ones within ~2 s — so once that settles a reconnecting call re-anchors at once, and a
   * relayed call starts its direct-upgrade punches over (the new network may be punchable).
   */
  void OnLocalNetworkChanged() override;
  /**
   * k6: the call's pair path policy (both mobility classes). Unset → the default policy
   * (Stationary pair: punch, upgrade, relay standby).
   */
  using PathPolicyProvider = std::function<CallPathPolicy(const std::string& call_id)>;
  void SetPathPolicyProvider(PathPolicyProvider provider) { path_policy_ = std::move(provider); }
  /** H013: the relays the call's answerer advertised in its CallAccept (the caller's reach tries them first). */
  using AdvertisedRelaysProvider = std::function<std::vector<std::string>(const std::string& call_id)>;
  void SetAdvertisedRelaysProvider(AdvertisedRelaysProvider provider) { advertised_relays_ = std::move(provider); }
  /**
   * H013: the caller of `call_id` stopped trying to reach us. An answerer still waiting for its circuit
   * stops too (ConnectFailed, as at its own limit); anything else ignores it.
   */
  void OnPeerGaveUpReaching(const std::string& call_id) override;
  /** Where the path reports its events (its parent binds it); its timers are delayed events. */
  void SetOutbox(OwnerOutbox<DirectPathEvent> outbox);
  /** Where this path reports to the calls it serves (events up; V055). */
  void SetReports(OwnerOutbox<DirectReport> reports) { reports_ = std::move(reports); }
  /** An event it reported, back from the calls owner's queue. */
  void Handle(DirectPathEvent& event) override;
  /** k6: a mobility class of the call flipped (calls owner): upgrade punches follow the new policy. */
  void OnPathPolicyChanged(const std::string& call_id) override;
  /** Wait after a network change before re-anchoring (production 2.5 s). */
  void SetNetworkSettleMsForTest(int delay_ms) { network_settle_ms_ = delay_ms; }
  /** Every relay-standby attempt after this delay (0 = production 5 s / 20 s / 60 s). */
  void SetRelayStandbyDelayMsForTest(int delay_ms) { standby_delay_ms_for_test_ = delay_ms; }
  /** Every direct-upgrade attempt after this delay (0 = production 3 s / 20 s / 60 s). */
  void SetDirectUpgradeDelayMsForTest(int delay_ms) { upgrade_delay_ms_for_test_ = delay_ms; }
  /** Test-only (calls owner): what the reach loop last settled on — may differ from the bound link. */
  void SetReachKindForTest(mesh::PeerLinkKind kind) { reach_kind_ = kind; }
  /** Shrink per-attempt ConnectAsync timeout (and watchdog margin) for gtests (0 = production default). */
  void SetConnectAttemptTimeoutMsForTest(int timeout_ms);

  /**
   * SoftMigrate: close 1:1 call-media stream without MediaEngine::Stop so SFU capture continues.
   * Prefer ReleaseDirectTransport(token) when a MediaSeat is wired.
   */
  void ReleaseDirectTransport() override;
  /** V036 Phase 3: token-gated SoftMigrate release (no-op when token not bound). */
  void ReleaseDirectTransport(const CallMediaSeat::Token& token) override;
  /** CallDirectDriver: the call's coordinator starts the 1:1 connect (offerer / answerer). */
  void ScheduleDirectStart(const std::string& call_id, const std::string& peer_identity, bool offerer) override;

  /**
   * Engine Stop — **seat teardown hook only** when MediaSeat is wired (V036).
   * CallSessionManager Leave/Accept must use seat.Release, not this. Calls owner (every stop reaches here
   * through the owner's events).
   */
  void StopMeshMedia(const std::string& call_id) override;
  /**
   * CallAccept/Invite taught PeerId→relay: (works for non-contacts). Rebind deferred inbound
   * on_audio stream_id when it matches the pending inbound PeerId.
   */
  void NotePeerIdRelayMapping(const std::string& peer_id, const std::string& relay_identity) override;

  /**
   * Abort in-flight Connect (`AbortConnectSequence` + connect Shutdown + Detach); late Connect
   * callbacks no-op on sequence. Must run before destroying this bridge / transport / mesh host.
   * The abort is synchronous; `timeout_ms` is accepted for existing callers and unused.
   * See THREADING.md Cancel / Abort contract (arm ⇒ complete on cancel).
   */
  void PrepareForTeardown(int timeout_ms = 0) override;
  void DetachReach() override;
  void AttachReach(const CallDirectPathDeps& deps) override;
  CallDirectPathStatus Status() const override;

  /** True while an async Connect sequence is in flight (shutdown measurement). */
  bool IsConnectWorkerInflight() const { return connect_.InFlight(); }

  /** True after PrepareForTeardown. */
  bool IsStopping() const { return stopping_.load(std::memory_order_acquire); }

  /** Keep dial/circuit pointers valid when ConversationsHub rewires deps (N025 listen sync). */
  void SetReachDeps(mesh::PeerReachPorts reach);
  /** Fire-and-forget bootstrap seed warm (CallStack::WarmBootstrapSeedSessions). */
  /** The connect sequence's Diag-line probe (calls owner); empty clears it. */
  void SetConnectDiagProbe(CallConnectEnvProbe probe);
  void SetSeedWarm(std::function<void()> warm);
  /** Both roles: park circuit reserve on org seed (CallStack::ReserveOnBootstrapSeeds). */
  void SetSeedReserve(std::function<void()> reserve);
  /**
   * Await at least one bootstrap/directory seed Connected before circuit/punch
   * (MeshConnectivity rendezvous EnsureBootstrapSeedParkedAsync). Forwarded to PeerReachCoordinator.
   */
  void SetSeedParkAwait(mesh::PeerReachCoordinator::SeedParkAwait park);

  void SetDirectArmingPorts(CallDirectArmingPorts ports);
  /** V036 exclusive media epoch — Stack installs; Bridge must not hold CallMediaSeat*. */
  void SetSeatPorts(CallDirectSeatPorts ports);

  /** Last successful 1:1 reach mode: direct | punched | circuit (empty before connect). */
  std::string MediaPathKind() const;
  /** The active 1:1 call-media link's figures (unavailable when no direct call is bound). */
  CallLinkCounters MediaLinkCounters() const;

  /** V039 Direct planner Apply — product callbacks on the calls owner. */
  void Apply(CallDirectPlannerEvent ev, const std::string& call_id = {},
             const std::string& peer_identity = {});
  CallDirectPlannerPhase DirectPlannerPhase() const { return direct_planner_phase_; }

private:
  Roe<void> BeginSession(const std::string& call_id, const std::string& peer_identity, bool offerer);
  // Start steps (each runs as its own event; the key poll is a delayed event per round).
  void RunOffererStart(const std::string& call_id, const std::string& peer_identity);
  void RunAnswererStart(const std::string& call_id, const std::string& peer_identity);
  void DeferAnswererUntilMediaKey(const std::string& call_id, const std::string& peer_identity,
                                  const std::string& reason);
  void OnKeyPollDue(const direct_event::KeyPollDue& due);
  void OnDeferredMediaKeyTimeout(const std::string& call_id);
  void StartDeferredAnswerer(const std::string& call_id);
  // BeginSession steps.
  void ResetDirectSessionState(const std::string& call_id, const std::string& peer_identity, bool offerer);
  /** Stop the prior engine session / connect; true when an inbound direct stream is kept. */
  bool StopPriorDirectAttempt(bool offerer);
  void ParkOnBootstrapSeed();
  Roe<void> StartDirectEngine(const std::string& call_id);
  void StartDirectConnect(const std::string& call_id, const std::string& peer_identity, bool offerer,
                          uint32_t media_epoch, const ByteVector& media_key);
  Roe<void> StartMediaAsOfferer(const std::string& call_id, const std::string& peer_identity);
  Roe<void> StartMediaAsAnswerer(const std::string& call_id, const std::string& peer_identity);
  /** Link to reach for this session's Connect (call roster → mesh keys, offerer → Reach). */
  mesh::PeerReachRequest BuildReachRequest(const CallMediaDirectConnectParams& params);
  /** Call-side reactions to the connect sequence (media key resend, path label, commit / fail). */
  CallMediaConnectHooks MakeConnectHooks(const std::string& call_id);
  /** Chrome ConnectFailed + Direct Idle; optionally StopMeshMedia (zombie TX / teardown). */
  void SurfaceConnectFailed(const std::string& call_id, const std::string& err, bool stop_media);
  /**
   * Bump the send generation (gates 1:1 TX) and abort the connect sequence (timers, pending
   * reach, InFlight cleared — THREADING.md Cancel / Abort contract).
   */
  void AbortConnectSequence();
  Roe<ByteVector> LoadActiveMediaKey(const std::string& call_id) const;
  /** Direct stream up: mark media connected when capture is live, always advance lifecycle/chrome. */
  void CommitDirectConnected(const std::string& call_id);
  /**
   * The connect sequence gave up (DecideConnectFailure): commit if the peer's redial restored direct
   * media, hold one short grace for its hello in progress (B44), else surface Failed — the call
   * stays open, and a later connection for it reconnects it (PeerReconnected).
   */
  void FailUnlessDirectRecovered(const std::string& call_id, const std::string& err, bool grace_used = false);
  /** UI: map an accepted inbound bundle's mesh PeerId to the roster identity / mixer stream. */
  void BindInboundPeer(const std::string& call_id, const std::string& inbound_peer_id);
  /** Callbacks for one bundle (either direction). fixed_stream 0 = inbound identity binding. */
  CallMediaDirectCallbacks MakeBundleCallbacks(const std::string& call_id, uint32_t fixed_stream,
                                               const char* label);
  /** UI: a bundle closed / failed — ignore during SoftMigrate / SFU attach, else ConnectFailed. */
  void OnBundleFailed(const std::string& call_id, const std::string& reason, CallMediaErr code);
  struct ReceiveGate;
  /** Transport I/O: hand a 1:1 frame to the engine (thread-safe), gated by `gate`. */
  static void ReceiveDirectMedia(ReceiveGate& gate, MediaEngine& media, const CallMediaHost& host,
                                 const OwnerOutbox<DirectPathEvent>& outbox, const std::string& call_id,
                                 uint32_t fixed_stream, uint8_t channel, uint32_t seq, uint8_t mark,
                                 const std::vector<uint8_t>& payload);
  void RebindInboundStream(const std::string& call_id);
  void ReleaseDirectTransportBody();
  /** NAT dogfood: dialable "direct" with TX-only → force circuit ensure + re-dial. */
  void MaybeEscalateTxOnlyDirect();
  void EscalateTxOnlyViaCircuit(const std::string& call_id, const std::string& peer);
  void SetDirectPlannerPhase(CallDirectPlannerPhase next, CallDirectPlannerEvent ev,
                             const std::string& call_id);
  CallDirectPlannerApplyContext BuildDirectPlannerContext(const std::string& call_id, const std::string& peer_identity);
  /** Arm health / TX-only / connect-timeout timer (pm3). */
  void ArmDirectHealthTimer();
  void CancelDirectHealthTimer();
  /** Renew relay reservations (15 s lease) while a media session is connecting / live (k2). */
  /** Direct transport has a leg in MediaReady (not merely a bundle in hello / AwaitingMedia). */
  bool DirectMediaReady() const;
  void ArmReserveRenewal();
  void CancelReserveRenewal();
  void OnReserveRenewFire();
  /** k3: while Live on a relayed path, punch for a direct link a few times (offerer drives). */
  void ArmDirectUpgrade(const std::string& call_id);
  void ScheduleDirectUpgrade();
  void CancelDirectUpgrade();
  void OnDirectUpgradeFire();
  /**
   * k6 (K003): a call Live on a direct / punched path keeps a relayed standby (offerer drives, when
   * the pair policy wants one): a circuit under the call, added with `path_add`. Best-effort — the
   * relay may refuse; retried a few times.
   */
  void ArmRelayStandby(const std::string& call_id);
  void ScheduleRelayStandby();
  void CancelRelayStandby();
  void OnRelayStandbyFire();
  /** k3-4: TX-only restart (Detach + BeginSession via circuit) — the fallback when the call cannot move. */
  void EscalateBreakBeforeMake(const std::string& call_id, const std::string& peer);
  void RestartAfterEscalate(const std::string& call_id, const std::string& peer);
  void CancelEscalateReach();
  /** k4: the offerer reaches the peer again and migrates the call onto that link (retries). */
  void ScheduleReanchor(const std::string& call_id, std::chrono::milliseconds delay);
  void Reanchor(const std::string& call_id);
  void CancelReanchor();
  /** k6: the transport's own relayed → direct move follows the pair policy (anchor = stay). */
  void ApplyPathPolicyToTransport(const std::string& call_id);
  CallPathPolicy PathPolicyFor(const std::string& call_id) const {
    return path_policy_ ? path_policy_(call_id) : CallPathPolicy{};
  }
  /** Amp PeerId for a call roster key (account: → PeerId); unchanged otherwise. */
  std::string ReachPeerIdFor(const std::string& key);
  /** `call_id`'s `CallMedia`; null (quietly) for a call not admitted here. */
  CallMedia* CallMediaFor(const std::string& call_id);
  /** The group path carries `call_id`'s media now (its coordinator says so). */
  bool HopAttachedFor(const std::string& call_id);
  /** Media flows on the call's path: this path's stream committed connected, or the hop carries it. */
  bool MediaConnected(const std::string& call_id);
  /** Stop the engine through `call_id`'s `CallMedia` (a leftover without one: directly). */
  void StopEngineFor(const std::string& call_id, const char* why);
  /** `call_id` is this path's call; else logs `what` and says no (a mis-routed call). */
  bool ForThisCall(const std::string& call_id, const char* what) const;
  /** The call peer's mesh PeerId for reach / circuit / upgrade (never a local dial alias). */
  std::string CallPeerMeshId();
  void OnDirectHealthTimerFire();

  /** The one call this path serves. */
  const std::string call_id_;
  CallMediaHost& host_;
  ICallSessionStore& sessions_;
  ICallMediaKeyStore& media_keys_;
  MediaEngine& media_;
  ICallMediaTransport& direct_;
  CallDirectArmingPorts arming_;
  OwnerOutbox<DirectReport> reports_;
  CallDirectSeatPorts seat_;
  std::function<void()> seed_warm_;
  std::function<void()> seed_reserve_;
  /** Peer link establishment (reach loop); owns no call state. */
  mesh::PeerReachCoordinator reach_;
  /** Connect attempts (reach + bundle open, watchdog, retry); owns no call product state. */
  CallMediaConnectCoordinator connect_;
  /** Link kind the last successful reach settled on (calls owner). */
  mesh::PeerLinkKind reach_kind_ = mesh::PeerLinkKind::Unknown;
  /** Next connect sequence must insist on a relayed link (TX-only escalate). */
  bool force_circuit_ensure_ = false;
  bool session_offerer_ = false;
  int64_t direct_connected_at_ms_ = 0;
  bool tx_only_escalation_done_ = false;

  std::string media_peer_identity_;
  /** This path's media session runs (BeginSession .. stop). */
  bool media_session_ = false;
  /** The answerer waits here for the call's key. */
  bool pending_answerer_ = false;
  std::string pending_answerer_peer_;
  /** Inbound hello PeerId while stream_id deferred (non-contact / pre-Accept). */
  std::string inbound_deferred_peer_id_;
  bool mesh_connect_failed_ = false;
  bool mesh_connect_missing_mic_ = false;
  /**
   * Outcome of the seed parks for the current connect (SeedParkOutcome). Written on the park's
   * completion thread, which may outlive this bridge; read by the UI state snapshot.
   */
  struct SeedParkState {
    std::atomic<bool> unreachable{false};
    std::atomic<uint64_t> epoch{0}; // bumped by ClearMeshConnectFailed
  };
  std::shared_ptr<SeedParkState> seed_park_state_ = std::make_shared<SeedParkState>();
  /** Bumped by AbortConnectSequence; the StartSfu send fn drops TX from an older generation. */
  std::atomic<uint64_t> connect_generation_{0};
  std::atomic<bool> stopping_{false};
  /** Names the current key wait; bumped when the pending (key-deferred) answerer changes. */
  uint64_t key_wait_gen_ = 0;
  OwnerOutbox<DirectPathEvent> outbox_;
  /** Steps waiting for a result from another thread (reach / upgrade / standby / migrate answers). */
  OwnerSteps steps_;
  /**
   * The callback to hand an async API: it only reports its result (any thread); `step` runs with it
   * on the owner, as the path's next event.
   */
  template <typename T>
  std::function<void(T)> OnOwner(std::function<void(T)> step) {
    const uint64_t id = steps_.StoreFor<T>(std::move(step));
    return [outbox = outbox_, id](T value) {
      outbox.Emit(direct_event::StepReady{OwnerStepReady{id, std::make_shared<std::any>(std::move(value))}});
    };
  }
  void OnPathChanged(const std::string& call_id, CallMediaLinkKind kind);
  uint64_t direct_health_timer_id_ = 0;
  uint64_t reserve_renew_timer_id_ = 0;
  uint64_t upgrade_timer_id_ = 0;
  bool standby_armed_ = false;
  int standby_attempt_ = 0;
  uint64_t standby_timer_id_ = 0;
  mesh::PeerReachId standby_reach_id_ = 0;
  int standby_delay_ms_for_test_ = 0;
  bool upgrade_armed_ = false;
  int upgrade_attempt_ = 0;
  int upgrade_delay_ms_for_test_ = 0;
  /** k3-4: circuit being built under a TX-only call (0 = none). */
  mesh::PeerReachId escalate_reach_id_ = 0;
  bool reanchor_armed_ = false;
  uint64_t reanchor_timer_id_ = 0;
  mesh::PeerReachId reanchor_reach_id_ = 0;
  int reanchor_retry_ms_ = 2000;
  /** Past Amp's network-change grace (2 s): the links left are the ones that answered. */
  int network_settle_ms_ = 2500;
  PathPolicyProvider path_policy_;
  AdvertisedRelaysProvider advertised_relays_;
  int64_t answerer_await_hold_ms_ = kCallAnswererAwaitHoldMs;
  /** Inside the 15 s StartReserve lease so consecutive leases overlap. */
  int reserve_renew_interval_ms_ = 10000;
  CallDirectPlannerPhase direct_planner_phase_ = CallDirectPlannerPhase::Idle;
  /**
   * This path's connect gave up while the call stays open (failed ≠ closed; LiveCall's Failed). Its
   * peer and role are the LiveCall's (P2pLiveCall). Set by SurfaceConnectFailed after the stop;
   * cleared by any other stop (Leave) or a new session.
   */
  struct FailedOpenCall {
    /** PeerReconnected already raised: a second inbound bundle must not queue a second resume. */
    bool resume_requested = false;
  };
  /** A 1:1 call's peer and this side's media role, from its LiveCall (placed → offerer). */
  struct CallPeerRole {
    std::string peer_identity;
    bool offerer = false;
  };
  std::optional<CallPeerRole> PeerRoleFromLiveCall(const std::string& call_id) const;
  std::optional<FailedOpenCall> failed_open_;
  int media_key_inbox_poll_rounds_ = 90;
  std::atomic<uint32_t> audio_seq_{0};
  /** What 1:1 receive reads on the transport's I/O thread; the owner publishes it. */
  struct ReceiveGate {
    /** Cleared when the bridge goes: frames still in flight on I/O drop. */
    std::atomic<bool> open{true};
    /** Inbound remote mixer stream; 0 = defer until the relay: identity is known (BeginSession). */
    std::atomic<uint32_t> remote_stream{0};
    /** Steady-clock ms before which I/O does not ask for another rebind (one in flight / just failed). */
    std::atomic<int64_t> rebind_not_before_ms{0};
    /** The call's "the hop carries its media" flag (its hop path writes it); null: no hop. Set before traffic. */
    std::shared_ptr<const std::atomic<bool>> hop_carries_media;
  };
  std::shared_ptr<ReceiveGate> receive_ = std::make_shared<ReceiveGate>();
};

} // namespace pbr
