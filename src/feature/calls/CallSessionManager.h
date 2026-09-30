#pragma once

#include "common/privacy/AddressDisclosure.h"
#include "foundation/crypto/IPskSessionStore.h"
#include "domain/media/CallMediaEngine.h"
#include "domain/messaging/CallControlCodec.h"
#include "domain/messaging/BroadcastJoinTicket.h"
#include "domain/messaging/CallLifecycleTypes.h"
#include "domain/messaging/CallSessionStore.h"
#include "foundation/data/PricingTypes.h"
#include "domain/messaging/InitiationBillingStore.h"
#include "common/thread/IThreadStore.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/IdentityStore.h"
#include "domain/messaging/CallMediaKeyStore.h"
#include "feature/calls/CallControlClient.h"
#include "feature/calls/CallDeliveryPorts.h"
#include "domain/mesh/reach/PeerMediaRelayCaps.h"
#include "domain/people/PeerAccountBook.h"
#include "feature/calls/CallInitiationBilling.h"
#include "feature/calls/CallMediaKeyExchange.h"
#include "feature/calls/CallReachSignals.h"
#include "feature/calls/CallSessionEvents.h"
#include "foundation/runtime/OwnerOutbox.h"
#include "feature/calls/CallMediaSeat.h"
#include "feature/calls/CallDirectPathDeps.h"
#include "feature/calls/CallMediaBridge.h"
#include "feature/calls/CallMediaHost.h"
#include "feature/calls/CallTopologyController.h"
#include "feature/calls/CallSessionWorkflow.h"
#include "feature/calls/SharedPorts.h"
#include "foundation/runtime/DeferredSelf.h"

#include "common/Error.h"
#include "common/Module.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class CallMediaBridge;

/**
 * Direct media façade for CallSessionManager (V042).
 * CSM must not hold CallMediaBridge* — ops copy these functions.
 */
struct CallDirectMediaPorts {
  std::function<std::string()> media_path_kind;
  std::function<CallLinkCounters()> media_link_counters;
  std::function<void(const std::string& peer_id, const std::string& relay_identity)>
      note_peer_id_relay_mapping;
  std::function<bool()> is_connect_failed;
  std::function<bool()> connect_missing_mic;
  std::function<void()> poll_connect_health;
  std::function<bool(const std::string& call_id)> media_attempted;
  std::function<void(const std::string& call_id)> note_media_attempted;
  std::function<void(const std::string& call_id)> on_media_key_ready;

  bool IsBound() const { return static_cast<bool>(media_path_kind); }
};

/**
 * MediaSeat façade for CallSessionManager (V043).
 * CSM must not hold CallMediaSeat* — ops copy these functions.
 */
struct CallMediaSeatPorts {
  std::function<void(const std::string& call_id)> release;

  bool IsBound() const { return static_cast<bool>(release); }
};

/**
 * Call session lifecycle façade (a2 / V014 / a4) — V036 Phase 3 **signaling** owner.
 * Duplex start/stop go through CallMediaSeat + CallDirectPath / CallHopPath; do not call
 * CallMediaBridge::StopMeshMedia or engine StartSfu from here when a seat is wired.
 * Topology + mesh media live in CallTopologyController / CallMediaBridge (path plugins).
 * Path façades take Ops only — no standing CallMediaBridge* / CallMediaSeat* (V048).
 */
class CallSessionManager : public Module, private CallMediaHost {
public:
  using RingChangedFn = std::function<void()>;
  using MediaRelayDeps = CallTopologyController::MediaRelayDeps;

  CallSessionManager(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                     CallSessionStore& sessions, CallMediaKeyStore& media_keys, CallDeliveryPorts delivery,
                     IPskSessionStore& psk_store, CallMediaEngine& media);

  void SetOnRingChanged(RingChangedFn callback);
  /** Second listener — mesh (N025 listen) must not overwrite UI chrome refresh. */
  void SetOnRingChangedMesh(RingChangedFn callback);
  using PrefetchPeerReachFn = std::function<void(const std::string& identity)>;
  void SetPrefetchPeerReachability(PrefetchPeerReachFn callback);
  /** Mesh circuit readiness (park/reserve) — composition projects the shared MeshConnectivity. */
  using EnsureCircuitReadyFn = std::function<void()>;
  void SetEnsureCircuitReady(EnsureCircuitReadyFn callback);
  /** AcceptInvite may await circuit-ready before CallAccept. */
  /** Async circuit park for the Accept gate; the composition posts `done` onto the calls owner. */
  using ParkCircuitFn = std::function<void(int timeout_ms, std::function<void(bool ready)> done)>;
  void SetParkCircuit(ParkCircuitFn park);
  /** The mesh's reach signals (H011 R1, H012 punch, K005 caps) carried to the active call's peer. */
  CallReachSignals& ReachSignals() { return reach_signals_; }
  /** Local `/ip4/…/tcp/…/p2p/…` listen set for call-control dial bootstrap. */
  using LocalListenMultiaddrsFn = std::function<std::vector<std::string>()>;
  void SetLocalListenMultiaddrsProvider(LocalListenMultiaddrsFn callback);
  /**
   * projects/privacy T1: call signalling carries our listen addresses, and we answer a signalling
   * punch, only for peers this gate allows. Null = everyone. Not owned.
   */
  void SetAddressDisclosure(const AddressDisclosureGate* gate) { address_disclosure_ = gate; }
  /** projects/privacy T3: who may call us; an invite from outside is dropped silently. Owner thread. */
  void SetInboundCallAudience(InboundAudience audience) { inbound_call_audience_ = audience; }
  /** Whether an invite from `caller` may ring (the audience; contacts from the disclosure gate). */
  bool AllowsInboundCall(const std::string& caller) const;
  /** Local capability ads for invite/accept (V030). */
  using LocalPeerCapsFn = std::function<CallPeerCaps()>;
  void SetLocalPeerCapsProvider(LocalPeerCapsFn callback);
  /** The remote's caps for a call — from invite / accept, then `call_caps_update` (K005). */
  using CallPeerCapsSink = std::function<void(const std::string& call_id, const CallPeerCaps& caps)>;
  void SetCallPeerCapsSink(CallPeerCapsSink sink);
  /** Local mesh PeerId (base58) for invite/accept — PeerId→relay without contacts. */
  using LocalMeshPeerIdFn = std::function<std::string()>;
  void SetLocalMeshPeerIdProvider(LocalMeshPeerIdFn callback);
  /** Register peer listen multiaddrs from invite/accept into the dial registry. */
  using RegisterPeerListenMultiaddrsFn =
      std::function<void(const std::string& identity, const std::vector<std::string>& multiaddrs)>;
  void SetRegisterPeerListenMultiaddrs(RegisterPeerListenMultiaddrsFn callback);
  /** Cache media_relay ads from invite/accept caps (keyed by mesh PeerId). */
  void NotePeerMediaRelayCap(const std::string& peer_id, bool media_relay);
  /**
   * Remember mesh PeerId ↔ relay: for call-media stream ids (contacts often lack PeerId —
   * PreferLocal dogfood: Moto contact had only relay: so inbound hashed PeerId ≠ SFU stream).
   */
  void NoteMeshPeerIdForRelay(const std::string& relay_identity, const std::string& peer_id);
  bool PeerHasMediaRelayCap(const std::string& peer_id) const;
  std::vector<std::string> ListMediaRelayCapablePeerIds() const;
  void SetMediaRelayDeps(MediaRelayDeps deps);
  /** Direct media ops (ScheduleStart / Retry / SoftMigrate release) — Stack installs from bridge. */
  void SetDirectMediaPorts(CallDirectMediaPorts ports);
  /** The 1:1 path's arming / progress ports over the calls' media progress (the stack binds the bridge). */
  CallDirectArmingPorts DirectArmingPorts();
  /** Seat ops (V043) — Stack installs; CSM must not hold CallMediaSeat*. */
  /** Topology Seat ports (V046) — Stack installs; Topology must not hold CallMediaSeat*. */
  /** Build CSM seat ports over owned topology_ (Stack / compose tests). */
  /** The seat the calls' media coordinators take (null: none bound — mesh stopped / harness). */
  /** The media seat (owned here: the calls' coordinators arbitrate it). Calls owner. */
  const CallMediaSeat& Seat() const { return seat_; }
  CallMediaSeat& SeatForTest() { return seat_; }
  // --- The 1:1 path (CallMediaBridge), owned here like the group path (topology) -------------------
  /**
   * Mesh media is wired: run the 1:1 path on `deps` — built on first use or a new transport, else
   * re-pointed at the new reach. Without a usable transport the path is dropped.
   */
  void AttachDirectPath(CallDirectPathDeps deps);
  /** Mesh media objects are being replaced: the path lets go of dial / reach (kept otherwise). */
  void DetachDirectPathReach();
  /** Before the transport stops: abort connects and let go of the path's streams. */
  void PrepareDirectPathForStop(int wait_ms = 0);
  /** After the transport stopped (or at teardown): drop the path. */
  void DropDirectPath();
  /** k5: the device's addresses moved. */
  void OnLocalNetworkMoved();
  /** k6: a call's path policy may have changed. */
  void OnPathPolicyChanged(const std::string& call_id);
  bool IsDirectConnectInFlight() const;
  CallMediaBridge* DirectPathForTest() { return direct_path_.get(); }
  /** The 1:1 path the calls' media coordinators start / release (null: mesh media not wired). */
  void SetDirectDriver(CallDirectDriver* direct) {
    direct_driver_ = direct;
    live_calls_.BindDirectDriver(direct);
  }
  /** A call's media coordinator (LiveCall); null for a call not admitted here. Calls owner. */
  CallMediaCoordinator* CallMedia(const std::string& call_id) { return live_calls_.Media(call_id); }
  /** Optional P001 initiation billing (outbound dial gate + inbound offer check). */
  void SetInitiationBillingStore(InitiationBillingStore* store);
  InitiationBillingStore* InitiationBilling() const { return billing_.Store(); }
  /** Offer amount stored for inviter when inbound invite carried pricing. */
  int64_t InitiationOfferMinorForPeer(const std::string& peer_identity) const;
  /** Set before AcceptClicked — consumed by AcceptInvite. */
  void SetPendingAcceptChargeDecision(InitiationChargeDecision decision);
  /** Set before AcceptClicked — consumed (and reset to false) by AcceptInvite. */
  void SetPendingAcceptVoiceOnly(bool voice_only);
  /** Expose private CallMediaHost base for bridge construction (MSVC-safe). */
  CallMediaHost& AsMediaHost() { return *this; }

  Roe<CallSession> StartCall(const std::string& origin_thread_id, bool video_allowed,
                             const std::vector<std::string>& invitee_identities);

  /** Accept (async: waits for the circuit park without blocking); `on_done` on the calls owner. */
  void AcceptInviteAsync(const std::string& call_id, std::function<void(Roe<void>)> on_done,
                         InitiationChargeDecision charge_decision = InitiationChargeDecision::Waive);
  Roe<void> DeclineInvite(const std::string& call_id);
  Roe<void> LeaveCall(const std::string& call_id, LiveCallEndReason reason = LiveCallEndReason::LocalLeave);
  // --- What the user does, and what the device shows (V037) ------------------------------------
  /**
   * A UI click or a media event for `call_id` (empty: the ring for Accept / Decline, else the active
   * call). Clicks run their session op; media events move the call's media progress. What the
   * device shows is projected from the calls (LiveCalls::Phase / Status). Calls owner.
   */
  void Apply(CallLifecycleEvent ev, const std::string& call_id = {});
  /** Where this subtree reports its events (the stack binds it at build). */
  void SetOutbox(OwnerOutbox<SessionEvent> outbox);
  /** An event this subtree reported, back from the calls owner's queue. */
  void Handle(SessionEvent& event);
  /** Runs after anything that can change what the device shows (phase, Status, the shown call). */
  void SetOnCallStateChanged(std::function<void()> fn) { on_call_state_changed_ = std::move(fn); }
  /** The call whose accept is in flight (clicked, or being accepted); empty if none. */
  std::string AcceptingCallId() const;
  /** Why the last accept failed; empty when it did not. */
  const std::string& LastError() const { return last_error_; }
  void ClearLastError() { last_error_.clear(); }

  /** The calls live on this device (admission → close). Calls owner only. */
  const LiveCalls& Live() const { return live_calls_; }
  /** Harnesses that seed store rows directly admit the call the way the workflow would. */
  LiveCalls& LiveCallsForTest() { return live_calls_; }
  /** Detach SFU + stop capture. Calls owner only — call before LeaveCall worker / app quit. */
  void StopCallMedia(const std::string& call_id);

  Roe<void> InviteParticipant(const std::string& call_id, const std::string& invitee_identity);

  Roe<std::vector<PendingCallInvite>> ListPendingInvites();
  Roe<std::optional<CallSession>> ActiveLocalCall() const;
  Roe<std::optional<PendingCallInvite>> TopPendingInvite();
  Roe<std::optional<PendingCallInvite>> PeekTopPendingInvite() const { return workflow_.PeekTopPendingInvite(); }

  Roe<std::optional<std::string>> PeerIdentityForCall(const std::string& call_id) const;
  Roe<std::optional<bool>> PeerVideoEnabledForCall(const std::string& call_id) const;
  Roe<std::optional<bool>> VideoAllowedForCall(const std::string& call_id) const;
  /**
   * True while a remote we invited is joined only through an implicit accept (B30): its CallAccept —
   * and with it a voice-only answer — has not arrived yet.
   */
  Roe<bool> AwaitingExplicitAnswerForCall(const std::string& call_id) const;
  Roe<std::vector<CallParticipant>> ListJoinedParticipants(const std::string& call_id) const;

  /**
   * V037: Lifecycle AcceptSucceeded (UI) re-arms answerer ScheduleStart when Status already
   * AllowsDirectPath — covers worker PostUI races that left seat bound Idle / no BeginSession.
   */
  void KickAnswererDirectMediaIfArmed(const std::string& call_id);

  bool IsAwaitingSfuRecovery() const;
  bool IsSoftMigrateInFlight() const;
  bool IsSfuAttachWaitActive() const;
  bool IsP2pConnectFailed() const;
  bool P2pConnectMissingMic() const;
  Roe<void> RetryP2pMedia(const std::string& call_id);
  /** A failed, open call: the peer's connection reached us — restart media keeping its stream. */
  Roe<void> ResumeP2pMedia(const std::string& call_id);
  /** Chrome heal when media already reports failed (not a UI-tick poll). */
  void PollP2pConnectHealth();

  std::optional<std::string> TakeLastMediaError();
  /** Non-mutating read (UI snapshot); the GUI takes it through the owner. */
  std::optional<std::string> PeekLastMediaError() const { return last_media_error_; }
  /** Clear only if still `seen` (the GUI showed it; a newer error stays). */
  void ClearLastMediaErrorIf(const std::string& seen);
  /** Latest hop/setup progress line for in-call chrome (empty when idle/connected). */
  std::string PeekMediaActivity() const;
  void ClearMediaActivity();

  Roe<std::optional<CallSession>> SessionForCall(const std::string& call_id) const;
  void SweepExpiredInvites();
  void AbandonOrphanedCallsAfterRestart();
  bool MediaAttemptedThisProcess(const std::string& call_id) const;

  Roe<void> ApplyInboundControl(ThreadMessage& message, const std::string& sender_identity,
                                std::optional<int64_t> relay_created_at_ms = std::nullopt,
                                std::optional<int64_t> relay_server_time_ms = std::nullopt);

  CallMediaEngine& Media();
  /** Combined hop health when SFU attached (empty otherwise). */
  CallHopHealth HopHealth() const;
  /** 1:1 reach path from CallMediaBridge (direct|punched|circuit); empty if unknown. */
  std::string MediaPathKind() const;
  CallLinkCounters MediaLinkCounters() const;
  bool IsSfuAttached() const;

  Roe<void> SetLocalAudioMuted(bool muted);
  /** `display_rotation_degrees` read on UI by the caller (L012). */
  Roe<void> SetLocalVideoEnabled(bool enabled, int display_rotation_degrees);
  /** Ask publisher for an IDR (empty identity = local encoder). */
  Roe<void> RequestVideoRefresh(const std::string& call_id, const std::string& publisher_identity);

  void ClearMediaCallbacks();

private:
  // Topology HostPorts helpers (V046 — not CallTopologyHost overrides)
  Roe<std::string> TopologyLocalIdentity() const;
  Roe<void> TopologyLeaveCall(const std::string& call_id);
  Roe<void> TopologyFanOutToJoined(const std::string& call_id, CallControlType type,
                                   const std::string& detail_json, const std::string& display,
                                   const std::string& skip_identity);
  Roe<void> TopologySendDirect(const std::string& peer_identity, CallControlType type,
                               const std::string& detail_json, const std::string& display);
  void TopologyNotifyRingChanged();
  void TopologySetLastMediaError(std::string message);
  void TopologySetMediaActivity(std::string message);
  void TopologyClearMediaActivity();
  void TopologyNoteMediaAttempted(const std::string& call_id);
  void TopologyClearMediaPeerIdentity();
  void TopologyRequestInboxSync();
  void BindTopologyHostPorts();

  // CallMediaHost
  Roe<std::string> P2pLocalIdentity() const override;
  Roe<void> P2pSendDirect(const std::string& peer_identity, CallControlType type,
                          const std::string& detail_json, const std::string& display) override;
  void P2pNotifyRingChanged() override;
  void P2pSetLastMediaError(std::string message) override;
  Roe<std::optional<std::string>> P2pPeerIdentityForCall(const std::string& call_id) const override;
  Roe<std::optional<std::string>> MeshPeerIdForAccount(const std::string& account) const override;
  Roe<std::optional<std::string>> RelayIdentityForMeshPeerId(const std::string& call_id,
                                                                  const std::string& peer_id) const override;
  void P2pResendMediaKey(const std::string& call_id, const std::string& peer_identity) override;
  void P2pRequestInboxSync() override;
  const LiveCall* P2pLiveCall(const std::string& call_id) const override { return live_calls_.Find(call_id); }
  CallMediaCoordinator* P2pCallMedia(const std::string& call_id) override { return live_calls_.Media(call_id); }
  bool HopCarriesMedia() const override;
  void P2pNoteInboundHello(const std::string& call_id, const std::string& identity,
                           const std::string& peer_id) override;

  Roe<void> MaybeRotateMediaKey(const std::string& call_id, const std::string& leaver_identity);
  Roe<void> EndCallLocal(CallSession& session, const std::optional<int64_t>& duration_ms, LiveCallEndReason reason);
  Roe<CallRosterDetail> BuildRosterDetail(const std::string& call_id) const;
  void NotifyRingChanged();

  void StopMediaIfCall(const std::string& call_id);
  void ScheduleStartDirectMedia(const std::string& call_id, const std::string& peer_identity, bool offerer);
  void BindWorkflowHostPorts();
  CallSessionWorkflow::WirePorts MakeWorkflowWirePorts();
  CallSessionWorkflow::HopPathPorts MakeWorkflowHopPorts();
  CallSessionWorkflow::ReachPorts MakeWorkflowReachPorts();
  void BindReachSignalPorts();
  void NotifyCallStateChanged();
  CallHopArmingPorts MakeHopArmingPorts();
  CallTopologySeatPorts MakeTopologySeatPorts();
  CallDirectSeatPorts DirectSeatPorts();
  CallDirectMediaPorts MakeDirectMediaPorts();
  void BindSeat();
  /** The seat released (or was taken over): stop that call's media. */
  void StopMediaForSeat(const std::string& call_id, uint64_t epoch_at_post, bool force);
  void ClickAccept(const std::string& call_id);
  void OnAcceptResult(const std::string& call_id, const Roe<void>& accepted);
  /** Our accept landed: kick the answerer's 1:1 start (and once more if it did not take). */
  void KickAnswererAfterAccept(const std::string& call_id);
  void ClickDecline(const std::string& call_id);
  void ClickLeave(const std::string& call_id);
  /** A failed, open call: Retry (this side dials) or resume over the peer's reconnect. */
  void RestartMedia(const std::string& call_id, bool resume);
  void RunMediaRestart(const std::string& call_id, bool resume);
  void RetryAnswererKick(const std::string& call_id);
  /** Flush deferred inbox/TailSync when no ActiveLocalCall remains. */
  void MaybeCatchUpAfterCall();

  // Inbound call-control arms — thin delegates to CallSessionWorkflow.
  Roe<void> HandleInboundInvite(const std::string& detail_json, const std::string& sender_identity,
                                const ThreadMessage& message, std::optional<int64_t> relay_created_at_ms,
                                std::optional<int64_t> relay_server_time_ms, const std::string& local_identity);
  Roe<void> HandleInboundAccept(const std::string& detail_json, const std::string& sender_identity,
                                const std::string& local_identity);
  Roe<void> HandleInboundDecline(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundLeave(const std::string& detail_json, const std::string& sender_identity,
                               const std::string& local_identity);
  Roe<void> HandleInboundRoster(const std::string& detail_json);
  Roe<void> HandleInboundSfuAttach(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundSfuAttachFailed(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundHopRefuse(const std::string& detail_json);
  Roe<void> HandleInboundVideoRefresh(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundEnded(const std::string& detail_json, const std::string& local_identity);


  ContactsStore& contacts_;
  CallSessionStore& sessions_;
  CallMediaKeyStore& media_keys_;
  CallDeliveryPorts delivery_;
  CallMediaEngine& media_;
  /** Before topology_ / live_calls_: both hold it (ports / media resources). */
  CallMediaSeat seat_;
  CallDirectDriver* direct_driver_ = nullptr;
  /** The 1:1 path's transport as attached (a new one rebuilds the path). */
  ICallMediaTransport* direct_transport_ = nullptr;
  /** Names the current 1:1 path: its events carry it (one built since drops the old ones). */
  uint64_t direct_path_generation_ = 0;
  CallTopologyController topology_;
  /** Before workflow_: the workflow drives it. */
  LiveCalls live_calls_;
  /** Outbound call-control I/O (after delivery_ / the stores it borrows). */
  CallControlClient control_;
  /** The call's media keys between the peers (after control_, which carries them). */
  CallMediaKeyExchange key_exchange_;
  /** P001 initiation pricing the workflow applies on invite / accept. */
  CallInitiationBilling billing_;
  /** Reach signals over call-control (after control_, their carrier). */
  CallReachSignals reach_signals_;
  CallSessionWorkflow workflow_;
  // Swapped at mesh start / stop and lifecycle bind; read as one snapshot per operation.
  SharedPorts<CallDirectMediaPorts> direct_media_;
  SharedPorts<CallMediaSeatPorts> media_seat_ports_;
  RingChangedFn on_ring_changed_;
  RingChangedFn on_ring_changed_mesh_;
  PrefetchPeerReachFn prefetch_reach_;
  EnsureCircuitReadyFn ensure_circuit_ready_;
  ParkCircuitFn park_circuit_;
  LocalListenMultiaddrsFn local_listen_multiaddrs_;
  const AddressDisclosureGate* address_disclosure_ = nullptr;
  InboundAudience inbound_call_audience_ = InboundAudience::Everyone;
  LocalPeerCapsFn local_peer_caps_;
  CallPeerCapsSink call_peer_caps_sink_;
  LocalMeshPeerIdFn local_mesh_peer_id_;
  RegisterPeerListenMultiaddrsFn register_peer_listen_multiaddrs_;
  /** PeerId → advertised media_relay (V030). Unknown = fail closed. */
  PeerMediaRelayCaps media_relay_caps_;
  /** mesh PeerId ↔ account learned from CallAccept / Invite / mDNS, over the contacts. */
  PeerAccountBook peer_accounts_;
  std::optional<std::string> last_media_error_;
  // --- UI intents (V037) ---
  /** Accept clicked and not answered yet (covers the click before the call is marked accepting). */
  std::string accept_in_flight_;
  std::string last_error_;
  std::function<void()> on_call_state_changed_;
  OwnerOutbox<SessionEvent> outbox_;
  /** The 1:1 path: last, so it goes first (it calls into the seat, LiveCalls and this manager). */
  std::unique_ptr<CallMediaBridge> direct_path_;
  std::string media_activity_;
};

} // namespace pbr
