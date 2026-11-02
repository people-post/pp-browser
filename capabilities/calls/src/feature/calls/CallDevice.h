#pragma once

#include "mesh/types/AddressDisclosure.h"
#include "lib/media/MediaEngine.h"
#include "lib/calls/CallControlCodec.h"
#include "lib/calls/CallLifecycleTypes.h"
#include "lib/calls/ICallSessionStore.h"
#include "feature/calls/CallReads.h"
#include "feature/calls/CallMeshGlue.h"
#include "lib/pricing/PricingTypes.h"
#include "lib/pricing/IInitiationBillingBook.h"
#include "common/thread/IThreadCatalog.h"
#include "common/thread/IThreadTranscript.h"
#include "lib/people/IContactBook.h"
#include "common/directory/ILocalAccount.h"
#include "lib/calls/ICallMediaKeyStore.h"
#include "feature/calls/CallControlClient.h"
#include "feature/calls/CallDeliveryPorts.h"
#include "mesh/reach/PeerMediaRelayCaps.h"
#include "lib/people/PeerAccountBook.h"
#include "feature/calls/CallInitiationBilling.h"
#include "lib/calls/CallMediaKeyExchange.h"
#include "feature/calls/CallReachSignals.h"
#include "feature/calls/CallSessionEvents.h"
#include "common/runtime/OwnerOutbox.h"
#include "lib/calls/CallMediaSeat.h"
#include "feature/calls/CallDirectPathDeps.h"
#include "feature/calls/CallMediaBridge.h"
#include "feature/calls/CallDirectPathQueries.h"
#include "feature/calls/CallTopologyController.h"
#include "feature/calls/CallSessionWorkflow.h"
#include "feature/calls/LiveCallMedia.h"
#include "feature/calls/SharedPorts.h"

#include "common/Error.h"
#include "common/Module.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

class CallMediaBridge;

/**
 * This device's calls, the parts (V056 device level; p2p-av-calls PHASES § cm4): it owns them and wires
 * them — the session workflow (the rows' one writer), the live calls and each call's media
 * (LiveCallMedia: a call's `CallMedia` owns its 1:1 and group paths, built here), the seat, the hop
 * service, the inbound media doorway, the control client, the media keys, billing and the reach
 * signals — and routes the events they report. It decides nothing the user asked for: the UI's
 * intents and inbound call control go through CallSessionManager, which uses these parts. Owned by
 * CallStack; calls owner only.
 */
class CallDevice : public Module, private CallDirectPathQueries {
public:
  using RingChangedFn = std::function<void()>;
  using MediaRelayDeps = CallTopologyController::MediaRelayDeps;

  CallDevice(const IThreadCatalog& threads, IThreadTranscript& transcript, IContactBook& contacts,
             const ILocalAccount& account, ICallSessionStore& sessions, const CallReads& reads,
             ICallMediaKeyStore& media_keys, CallDeliveryPorts delivery, MediaEngine& media);

  // --- The parts (CallSessionManager drives them) ----------------------------------------------
  const ICallSessionReader& Sessions() const { return sessions_; }
  const CallReads& Reads() const { return reads_; }
  CallSessionWorkflow& Workflow() { return workflow_; }
  /** The calls live on this device (admission → close). */
  const LiveCalls& Live() const { return live_calls_; }
  LiveCalls& Live() { return live_calls_; }
  /** Each live call's media. */
  LiveCallMedia& CallMediaOf() { return call_media_; }
  CallControlClient& Control() { return control_; }
  CallMediaKeyExchange& KeyExchange() { return key_exchange_; }
  /** The mesh's reach signals (H011 R1, H012 punch, K005 caps) carried to the active call's peer. */
  CallReachSignals& ReachSignals() { return reach_signals_; }
  const CallDeliveryPorts& Delivery() const { return delivery_; }
  /** The one media engine (the router reads whether it runs). */
  const MediaEngine& Engine() const { return media_; }
  /** The media seat (owned here: the calls' media arbitrate it). */
  const CallMediaSeat& Seat() const { return seat_; }
  CallMediaSeat& SeatForTest() { return seat_; }
  /** Harnesses that seed store rows directly admit the call the way the workflow would. */
  LiveCalls& LiveCallsForTest() { return live_calls_; }

  // --- Wiring (the stack) ----------------------------------------------------------------------
  /** Where the parts report their events (the stack binds it at build). */
  void SetOutbox(OwnerOutbox<SessionEvent> outbox);
  /** An event a part reported, back from the calls owner's queue. */
  void Handle(SessionEvent& event);
  void SetOnRingChanged(RingChangedFn callback);
  /** Second listener — mesh (N025 listen) must not overwrite UI chrome refresh. */
  void SetOnRingChangedMesh(RingChangedFn callback);
  /** Runs after anything that can change what the device shows (phase, Status, the shown call). */
  void SetOnCallStateChanged(std::function<void()> fn) { on_call_state_changed_ = std::move(fn); }
  /** What calls asks of this node's mesh and tells it (the stack's, once at build). */
  void SetMeshGlue(CallMeshGlue glue) { mesh_ = std::move(glue); }
  /** The glue, for a harness that adjusts one of it after the build. */
  CallMeshGlue& MeshGlue() { return mesh_; }
  void SetMediaRelayDeps(MediaRelayDeps deps);
  /** Optional P001 initiation billing (outbound dial gate + inbound offer check). */
  void SetInitiationBillingStore(IInitiationBillingBook* store);
  /** Hub teardown: drop queued follow-ups (a roster fan-out) before the store and delivery go. */
  void DropFollowUps();
  /** Expose the private CallDirectPathQueries base for the 1:1 path's construction (MSVC-safe). */
  CallDirectPathQueries& AsDirectPathQueries() { return *this; }

  // --- The 1:1 path's device-level deps --------------------------------------------------------
  /**
   * Mesh media is wired: build the calls' 1:1 paths on `deps` (a new transport drops the paths built
   * so far and the doorway; else they are re-pointed at the new reach). Without a usable transport
   * the paths are dropped.
   */
  void AttachDirectPath(CallDirectPathDeps deps);
  /** Mesh media objects are being replaced: the paths let go of dial / reach (kept otherwise). */
  void DetachDirectPathReach();
  /** Before the transport stops: abort connects and let go of the paths' streams. */
  void PrepareDirectPathForStop(int wait_ms = 0);
  /** After the transport stopped (or at teardown): drop the paths. */
  void DropDirectPath();
  /** The 1:1 path's arming / progress ports over the calls' media progress. */
  CallDirectArmingPorts DirectArmingPorts();
  /** `call_id`'s 1:1 path if built (tests). */
  CallMediaBridge* DirectPathForTest(const std::string& call_id);
  /** Build calls' 1:1 paths with `make` (empty: the product's); drops the paths built so far (tests). */
  void SetDirectPathFactoryForTest(CallDirectPathFactory make);

  // --- The mesh around the calls ---------------------------------------------------------------
  /** k5: the device's addresses moved. */
  void OnLocalNetworkMoved();
  /** k6: a call's path policy may have changed. */
  void OnPathPolicyChanged(const std::string& call_id);
  /** Cache media_relay ads from invite/accept caps (keyed by mesh PeerId). */
  void NotePeerMediaRelayCap(const std::string& peer_id, bool media_relay);
  /**
   * Remember mesh PeerId ↔ relay: for call-media stream ids (contacts often lack PeerId —
   * PreferLocal dogfood: Moto contact had only relay: so inbound hashed PeerId ≠ SFU stream).
   */
  void NoteMeshPeerIdForRelay(const std::string& relay_identity, const std::string& peer_id);
  bool PeerHasMediaRelayCap(const std::string& peer_id) const;
  std::vector<std::string> ListMediaRelayCapablePeerIds() const;

  // --- Media and call steps the router asks for ------------------------------------------------
  Roe<void> LeaveCall(const std::string& call_id, LiveCallEndReason reason = LiveCallEndReason::LocalLeave);
  Roe<std::string> LocalIdentity() const;
  Roe<CallRosterDetail> BuildRosterDetail(const std::string& call_id) const;
  Roe<void> SendDirect(const std::string& peer_identity, CallControlType type, const std::string& detail_json,
                       const std::string& display);
  void NotifyRingChanged();
  void NotifyCallStateChanged();
  void StopMediaIfCall(const std::string& call_id);
  void ScheduleStartDirectMedia(const std::string& call_id, const std::string& peer_identity, bool offerer);
  /** Our accept landed: kick the answerer's 1:1 start (and once more if it did not take). */
  void KickAnswererAfterAccept(const std::string& call_id);
  /**
   * V037: Lifecycle AcceptSucceeded (UI) re-arms answerer ScheduleStart when Status already
   * AllowsDirectPath — covers worker PostUI races that left seat bound Idle / no BeginSession.
   */
  void KickAnswererDirectMediaIfArmed(const std::string& call_id);
  /** A failed, open call: Retry (this side dials) or resume over the peer's reconnect. */
  void RestartMedia(const std::string& call_id, bool resume);
  /** What the direct path reports when the peer's connection for a failed call reaches us. */
  void PeerReconnectedForTest(const std::string& call_id) { RestartMedia(call_id, true); }

  // --- What the device shows (the stack publishes it) ------------------------------------------
  /** The call whose accept is in flight (clicked, or being accepted); empty if none. */
  std::string AcceptingCallId() const;
  /** Why the last accept failed; empty when it did not. */
  const std::string& LastError() const { return last_error_; }
  std::string& LastErrorMutable() { return last_error_; }
  void ClearLastError() { last_error_.clear(); }
  int64_t AloneUntilMs(const std::string& call_id) const { return workflow_.AloneUntilMs(call_id); }
  bool IsAwaitingSfuRecovery() const;
  /** Why the hop path is being recovered (the screen's status line). */
  CallRecovery Recovery() const;
  bool IsP2pConnectFailed() const;
  bool P2pConnectMissingMic() const;
  bool P2pConnectSeedUnreachable() const;
  bool IsDirectConnectInFlight() const;
  std::optional<std::string> TakeLastMediaError();
  /** Non-mutating read (UI snapshot); the GUI takes it through the owner. */
  std::optional<std::string> PeekLastMediaError() const { return last_media_error_; }
  /** Clear only if still `seen` (the GUI showed it; a newer error stays). */
  void ClearLastMediaErrorIf(const std::string& seen);
  /** The shown call's media activity line is dismissed (it lives on the call, V057 3). */
  void ClearMediaActivity();
  /** Combined hop health when SFU attached (empty otherwise). */
  CallHopHealth HopHealth() const;
  /** 1:1 reach path from CallMediaBridge (direct|punched|circuit); empty if unknown. */
  std::string MediaPathKind() const;
  CallLinkCounters MediaLinkCounters() const;
  bool IsSfuAttached() const;

private:
  // Effects the owned parts ask for (topology / workflow host ports, CallMediaHost): one body each
  Roe<void> TopologyLeaveCall(const std::string& call_id);
  Roe<void> TopologyFanOutToJoined(const std::string& call_id, CallControlType type, const std::string& detail_json,
                                   const std::string& display, const std::string& skip_identity);
  void SetLastMediaError(std::string message);
  void TopologyNoteMediaAttempted(const std::string& call_id);
  void RequestInboxSync();
  CallTopologyController::HostPorts MakeTopologyHostPorts();
  /** What a call's group path reported (events up): move the call's state, or act for it. */
  void OnHopReport(const HopReport& report);

  // CallDirectPathQueries (the 1:1 path's questions)
  const LiveCall* LiveCallOf(const std::string& call_id) const override { return live_calls_.Find(call_id); }
  CallMedia* CallMediaOf(const std::string& call_id) override { return call_media_.Media(call_id); }
  Roe<std::optional<std::string>> PeerIdentityForCall(const std::string& call_id) const override;
  Roe<std::optional<std::string>> MeshPeerIdForAccount(const std::string& account) const override;
  Roe<std::optional<std::string>> RelayIdentityForMeshPeerId(const std::string& call_id,
                                                             const std::string& peer_id) const override;
  // What the 1:1 path reported (DirectReport)
  void ResendMediaKey(const std::string& call_id, const std::string& peer_identity);
  void ApplyInboundHello(const std::string& call_id, const std::string& identity, const std::string& peer_id);

  Roe<void> MaybeRotateMediaKey(const std::string& call_id, const std::string& leaver_identity);
  Roe<void> EndCallLocal(CallSession& session, const std::optional<int64_t>& duration_ms, LiveCallEndReason reason);
  /** Build the inbound doorway on `transport` (with the 1:1 path that takes its bundles). */
  void BuildDoorway(ICallMediaTransport& transport);
  CallMediaInboundPorts MakeDoorwayPorts();
  /** What the 1:1 path reported (events up): move the call's state, or call the path down. */
  void OnDirectReport(const DirectReport& report);
  void OnAnswererKeyReady(const std::string& call_id, const std::string& peer_identity);
  void BindWorkflowHostPorts();
  CallSessionWorkflow::WirePorts MakeWorkflowWirePorts();
  /** What the session workflow reported (events up). */
  void OnWorkflowReport(const WorkflowReport& report);
  CallSessionWorkflow::ReachPorts MakeWorkflowReachPorts();
  void BindReachSignalPorts();
  CallHopArmingPorts MakeHopArmingPorts();
  CallTopologySeatPorts MakeTopologySeatPorts();
  CallDirectSeatPorts DirectSeatPorts();
  /** The device-level 1:1 deps (V056): each call's path is built from them. */
  std::unique_ptr<CallDirectDriver> MakeDirectPath(const std::string& call_id, uint64_t serial,
                                                   std::shared_ptr<CallEngine> engine,
                                                   std::shared_ptr<std::atomic<bool>> hop_carries_media);
  /** Each call's group path is built from the device's hop service (V056). */
  std::unique_ptr<CallHopDriver> MakeHopPath(const std::string& call_id, uint64_t serial,
                                             std::shared_ptr<CallEngine> engine,
                                             std::shared_ptr<std::atomic<bool>> hop_carries_media);

  /** What the screen reads of the active call's media paths. */
  CallMedia::View ActivePathsView() const;
  void BindSeat();
  /** The seat released (or was taken over): stop that call's media. */
  void StopMediaForSeat(const std::string& call_id, uint64_t epoch_at_post, bool force);
  void RunMediaRestart(const std::string& call_id, bool resume);
  Roe<void> RetryP2pMedia(const std::string& call_id);
  /** A failed, open call: the peer's connection reached us — restart media keeping its stream. */
  Roe<void> ResumeP2pMedia(const std::string& call_id);
  void RetryAnswererKick(const std::string& call_id);
  /** Flush deferred inbox/TailSync when no ActiveLocalCall remains. */
  void MaybeCatchUpAfterCall();

  /** H013: we placed this 1:1 call and our connect gave up — tell the peer. */
  void SendConnectGiveUp(const std::string& call_id, const std::string& peer_identity);

  IContactBook& contacts_;
  /** Read-only here: the workflow writes the rows (V055). */
  const ICallSessionReader& sessions_;
  /** The calls' durable reads (the stack's; the screen asks the same). */
  const CallReads& reads_;
  ICallMediaKeyStore& media_keys_;
  CallDeliveryPorts delivery_;
  MediaEngine& media_;
  /** Before live_calls_ and the calls' media (they hold it: ports / media resources). */
  CallMediaSeat seat_;
  uint64_t doorway_generation_ = 0;
  /** The device's hop side (V056): relay deps, hop ranking and planning; the hop path reads it. */
  CallHopService hop_service_;
  /** Before workflow_: the workflow drives it. */
  LiveCalls live_calls_;
  /** Each live call's `CallMedia` (after live_calls_, which it reads). */
  LiveCallMedia call_media_{live_calls_};
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
  RingChangedFn on_ring_changed_;
  RingChangedFn on_ring_changed_mesh_;
  CallMeshGlue mesh_;
  /** PeerId → advertised media_relay (V030). Unknown = fail closed. */
  mesh::PeerMediaRelayCaps media_relay_caps_;
  /** mesh PeerId ↔ account learned from CallAccept / Invite / mDNS, over the contacts. */
  PeerAccountBook peer_accounts_;
  std::optional<std::string> last_media_error_;
  /** Why the last accept failed (the workflow's ports write it). */
  std::string last_error_;
  std::function<void()> on_call_state_changed_;
  OwnerOutbox<SessionEvent> outbox_;
  /** The device's doorway for inbound call media, on the 1:1 path's transport (V056). */
  std::unique_ptr<CallMediaDoorway> doorway_;
  /** What each call's 1:1 path is built from (device level, V056); empty: none attached. */
  CallDirectPathDeps direct_deps_;
};

} // namespace pbr
