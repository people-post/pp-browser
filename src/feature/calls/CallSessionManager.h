#pragma once

#include "foundation/crypto/IPskSessionStore.h"
#include "domain/media/CallMediaEngine.h"
#include "domain/messaging/CallControlCodec.h"
#include "domain/messaging/AnnounceLiveJoin.h"
#include "domain/messaging/BroadcastJoinTicket.h"
#include "domain/messaging/CallSessionStore.h"
#include "foundation/data/PricingTypes.h"
#include "domain/messaging/InitiationBillingStore.h"
#include "common/thread/IThreadStore.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/IdentityStore.h"
#include "domain/messaging/CallMediaKeyStore.h"
#include "feature/calls/CallDeliveryPorts.h"
#include "feature/calls/CallMediaSeat.h"
#include "feature/calls/CallMediaHost.h"
#include "feature/calls/BroadcastSessionCoordinator.h"
#include "feature/calls/CallTopologyController.h"
#include "feature/calls/CallSessionWorkflow.h"

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
  std::function<void(const std::string& call_id, const std::string& peer_identity, bool offerer)>
      schedule_start;
  std::function<std::string()> media_path_kind;
  std::function<void(const std::string& peer_id, const std::string& relay_identity)>
      note_peer_id_relay_mapping;
  std::function<void(const std::string& call_id)> stop_mesh_media;
  std::function<bool()> is_connect_failed;
  std::function<bool()> connect_missing_mic;
  std::function<void()> poll_connect_health;
  std::function<Roe<void>(const std::string& call_id)> retry_mesh_media;
  std::function<bool(const std::string& call_id)> media_attempted;
  std::function<void(const std::string& call_id)> note_media_attempted;
  std::function<void()> release_direct_transport;
  std::function<void(const std::string& call_id)> on_media_key_ready;

  bool IsBound() const { return static_cast<bool>(schedule_start); }
};

/**
 * Lifecycle façade for CallSessionManager (V043).
 * CSM must not hold CallLifecycle* — ops copy these functions.
 */
struct CallSessionLifecyclePorts {
  std::function<bool()> allows_direct_path;
  std::function<const char*()> status_name;
  std::function<const char*()> armed_planner_name;
  std::function<void(const std::string& call_id)> set_direct_connecting;
  std::function<std::string()> accepting_call_id;
  std::function<std::string()> active_call_id;
  std::function<void(const std::string& call_id)> apply_remote_ended;
  std::function<bool()> is_outbound_calling;

  bool IsBound() const { return static_cast<bool>(allows_direct_path); }
};

/**
 * MediaSeat façade for CallSessionManager (V043).
 * CSM must not hold CallMediaSeat* — ops copy these functions.
 */
struct CallMediaSeatPorts {
  std::function<void(const std::string& call_id)> release;
  std::function<void(const std::string& call_id)> bind_hop_for_attach;

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
  /** Mesh circuit readiness (park/reserve) — composition projects CallMediaPlane. */
  using EnsureCircuitReadyFn = std::function<void()>;
  void SetEnsureCircuitReady(EnsureCircuitReadyFn callback);
  /** AcceptInvite may await circuit-ready before CallAccept. */
  using AwaitCircuitReadyFn = std::function<bool(int timeout_ms)>;
  void SetAwaitCircuitReady(AwaitCircuitReadyFn callback);
  /** H011 L3.1c: inbound call_circuit_r1 → answerer PreferLateReserve. */
  using PreferLateReserveFn = std::function<void(const std::string& relay_peer_id)>;
  void SetPreferLateReserve(PreferLateReserveFn callback);
  /**
   * H011 L3.1c: dialer announces chosen R1 to the active call peer (best-effort).
   * No-op when no active call / peer / empty R1.
   */
  void AnnounceCircuitR1(const std::string& circuit_r1_peer_id);
  /** Local `/ip4/…/tcp/…/p2p/…` listen set for call-control dial bootstrap. */
  using LocalListenMultiaddrsFn = std::function<std::vector<std::string>()>;
  void SetLocalListenMultiaddrsProvider(LocalListenMultiaddrsFn callback);
  /** Local capability ads for invite/accept (V030). */
  using LocalPeerCapsFn = std::function<CallPeerCaps()>;
  void SetLocalPeerCapsProvider(LocalPeerCapsFn callback);
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
  /** Lifecycle ops (V043) — Stack installs; CSM must not hold CallLifecycle*. */
  void SetLifecyclePorts(CallSessionLifecyclePorts ports);
  /** Topology hop arming ports (V048) — Stack installs; Topology must not hold CallLifecycle*. */
  void SetTopologyHopArmingPorts(CallHopArmingPorts ports);
  /** Seat ops (V043) — Stack installs; CSM must not hold CallMediaSeat*. */
  void SetMediaSeatPorts(CallMediaSeatPorts ports);
  /** Topology Seat ports (V046) — Stack installs; Topology must not hold CallMediaSeat*. */
  void SetTopologySeatPorts(CallTopologySeatPorts ports);
  /** Build CSM seat ports over owned topology_ (Stack / compose tests). */
  CallMediaSeatPorts MakeSeatPorts(CallMediaSeat* seat);
  /** Seat teardown hook: topology detach without re-entering seat.Release. */
  void TopologyOnMediaStoppedForSeat(const std::string& call_id);
  /** Optional P001 initiation billing (outbound dial gate + inbound offer check). */
  void SetInitiationBillingStore(InitiationBillingStore* store);
  InitiationBillingStore* InitiationBilling() const { return workflow_.InitiationBilling(); }
  /** Offer amount stored for inviter when inbound invite carried pricing. */
  int64_t InitiationOfferMinorForPeer(const std::string& peer_identity) const;
  /** Set before AcceptClicked — consumed by AcceptInvite. */
  void SetPendingAcceptChargeDecision(InitiationChargeDecision decision);
  /** Expose private CallMediaHost base for bridge construction (MSVC-safe). */
  CallMediaHost& AsMediaHost() { return *this; }

  Roe<CallSession> StartCall(const std::string& origin_thread_id, bool video_allowed,
                             const std::vector<std::string>& invitee_identities);

  Roe<void> AcceptInvite(const std::string& call_id,
                         InitiationChargeDecision charge_decision = InitiationChargeDecision::Waive);
  /**
   * Spine C (slice 1): arm a pending invite + ringing session from a live-join plan
   * Thin delegate to BroadcastSessionCoordinator (no SoftMigrate / media).
   */

  /** Broadcast live-announce arm/accept (Spine C) — prefer over SoftMigrate call paths. */
  BroadcastSessionCoordinator& Broadcast() { return broadcast_; }
  const BroadcastSessionCoordinator& Broadcast() const { return broadcast_; }

  Roe<PendingCallInvite> ArmJoinFromLiveAnnounce(const AnnounceLiveJoinPlan& plan,
                                                 const ArmLiveAnnounceJoinOpts& opts = {});

  /**
   * Spine C: accept an armed live-announce invite without SoftMigrate or 1:1 media.
   * Attaches SFU when session/pending carries sfu_hint (tip.hop_peer_id); otherwise
   * marks joined and defers media.
   */
  Roe<void> AcceptLiveAnnounceJoin(const std::string& call_id);

  Roe<void> DeclineInvite(const std::string& call_id);
  Roe<void> LeaveCall(const std::string& call_id);
  /** Detach SFU + stop SDL. UI thread only — call before LeaveCall worker / app quit. */
  void StopCallMedia(const std::string& call_id);

  Roe<void> InviteParticipant(const std::string& call_id, const std::string& invitee_identity);

  Roe<std::vector<PendingCallInvite>> ListPendingInvites();
  Roe<std::optional<CallSession>> ActiveLocalCall() const;
  Roe<std::optional<PendingCallInvite>> TopPendingInvite();

  Roe<std::optional<std::string>> PeerIdentityForCall(const std::string& call_id) const;
  Roe<std::optional<bool>> PeerVideoEnabledForCall(const std::string& call_id) const;
  Roe<std::optional<bool>> VideoAllowedForCall(const std::string& call_id) const;
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
  /** Chrome heal when media already reports failed (not a UI-tick poll). */
  void PollP2pConnectHealth();

  std::optional<std::string> TakeLastMediaError();
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
  bool IsSfuAttached() const;

  Roe<void> SetLocalAudioMuted(bool muted);
  Roe<void> SetLocalVideoEnabled(bool enabled);
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
  void TopologyBindMediaCallId(const std::string& call_id);
  void TopologyClearMediaPeerIdentity();
  void TopologyReleaseDirectMedia();
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
  bool P2pIsAwaitingSfuRecovery() const override;
  bool P2pExpectGroupSfuMigration(const std::string& call_id) const override;
  void P2pNoteExpectSfuAttach(const std::string& call_id) override;
  bool P2pIsSfuAttached() const override;
  void P2pClearAwaitingSfuRecovery() override;
  void P2pResendMediaKey(const std::string& call_id, const std::string& peer_identity) override;
  void P2pRequestInboxSync() override;

  Roe<std::string> LocalRelayIdentity() const;
  /** Mint/find e2e_public control DM before SoftMigrate / MediaKey fan-out (catalog warm). */
  Roe<std::string> EnsureCallControlThread(const std::string& peer_identity);
  Roe<void> SendCallDirectMessage(const std::string& peer_identity, CallControlType type,
                                  const std::string& detail_json, const std::string& display);
  Roe<void> AppendOriginHistory(const std::string& thread_id, CallControlType type, const std::string& text,
                                const std::string& detail_json);
  Roe<void> FanOutToJoined(const std::string& call_id, CallControlType type, const std::string& detail_json,
                           const std::string& display, const std::string& skip_identity);
  /** Fan-out to Joined and Ringing (and Invited) participants — used when ending so invitees clear. */
  Roe<void> FanOutToJoinedAndRinging(const std::string& call_id, CallControlType type,
                                     const std::string& detail_json, const std::string& display,
                                     const std::string& skip_identity);
  Roe<void> MaybeRotateMediaKey(const std::string& call_id, const std::string& leaver_identity);
  Roe<void> EndCallLocal(CallSession& session, const std::optional<int64_t>& duration_ms);
  Roe<CallRosterDetail> BuildRosterDetail(const std::string& call_id) const;
  void NotifyRingChanged();

  Roe<ByteVector> ResolvePeerSessionKey(const std::string& peer_identity) const;
  Roe<void> SendMediaKeyToPeer(const std::string& call_id, const std::string& peer_identity,
                               uint32_t media_epoch, const std::string& media_key_id, const ByteVector& key_bytes);
  void StopMediaIfCall(const std::string& call_id);
  Roe<void> LeaveCallIfActiveExcept(const std::string& keep_call_id);
  void ScheduleStartDirectMedia(const std::string& call_id, const std::string& peer_identity, bool offerer);
  void BindWorkflowHostPorts();
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
  Roe<void> HandleInboundMediaKey(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundSfuAttach(const std::string& detail_json);
  Roe<void> HandleInboundSfuAttachFailed(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundHopRefuse(const std::string& detail_json);
  Roe<void> HandleInboundVideoRefresh(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundCircuitR1(const std::string& detail_json);
  Roe<void> HandleInboundEnded(const std::string& detail_json, const std::string& local_identity);

  IThreadStore& store_;
  ContactsStore& contacts_;
  IdentityStore& identity_;
  CallSessionStore& sessions_;
  CallMediaKeyStore& media_keys_;
  CallDeliveryPorts delivery_;
  IPskSessionStore& psk_store_;
  CallMediaEngine& media_;
  CallTopologyController topology_;
  BroadcastSessionCoordinator broadcast_;
  CallSessionWorkflow workflow_;
  CallDirectMediaPorts direct_media_;
  CallSessionLifecyclePorts lifecycle_ports_;
  CallMediaSeatPorts media_seat_ports_;
  RingChangedFn on_ring_changed_;
  RingChangedFn on_ring_changed_mesh_;
  PrefetchPeerReachFn prefetch_reach_;
  EnsureCircuitReadyFn ensure_circuit_ready_;
  AwaitCircuitReadyFn await_circuit_ready_;
  PreferLateReserveFn prefer_late_reserve_;
  LocalListenMultiaddrsFn local_listen_multiaddrs_;
  LocalPeerCapsFn local_peer_caps_;
  LocalMeshPeerIdFn local_mesh_peer_id_;
  RegisterPeerListenMultiaddrsFn register_peer_listen_multiaddrs_;
  /** PeerId → advertised media_relay (V030). Absent key = unknown / fail closed. */
  std::unordered_map<std::string, bool> peer_media_relay_caps_;
  /** mesh PeerId → relay: identity learned from CallAccept/Invite listen multiaddrs / mDNS. */
  std::unordered_map<std::string, std::string> peer_id_to_relay_;
  /**
   * AutoKey key_init from ensure_peer_session_key — attached on the next SendCallDirectMessage
   * for that peer; kept until send succeeds so retries still carry key_init.
   */
  mutable std::mutex pending_call_key_init_mutex_;
  mutable std::unordered_map<std::string, std::string> pending_call_key_init_;
  std::optional<std::string> last_media_error_;
  std::string media_activity_;
};

} // namespace pbr
