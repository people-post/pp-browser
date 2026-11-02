#pragma once

#include "domain/call_media/ICallMediaTransport.h"
#include "mesh/reach/PeerReachCoordinator.h"
#include "lib/calls/CallTypes.h"
#include "lib/calls/CallDirectPlannerLogic.h"
#include "lib/calls/CallHopPlannerLogic.h"
#include "feature/calls/CallMediaInboundReply.h"
#include "common/runtime/OwnerSteps.h"

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/** What the calls' parts (CallDevice's subtree) report through the calls owner's queue. */
namespace session_event {

/** Our accept landed: kick the answerer's 1:1 start once more (the first kick can precede its arming). */
struct AnswererKickRetry {
  std::string call_id;
};
/** A failed, open call restarts its media (Retry, or resume over the peer's reconnect). */
struct MediaRestart {
  std::string call_id;
  bool resume = false;
};

} // namespace session_event

/** The session workflow's own events. */
namespace workflow_event {

/** After our accept reported: roster to the inviter and the rest, prefetch the inviter's reach. */
struct RosterAfterAccept {
  std::string call_id;
  std::string inviter;
  std::string local_identity;
  /** The workflow's follow-up epoch when reported (dropped once its follow-ups were cleared). */
  uint64_t epoch = 0;
};
/** After a peer's accept started media: roster fan-out, prefetch the peer's reach. */
struct RosterAfterRemoteAccept {
  std::string call_id;
  std::string peer;
  std::string local_identity;
  uint64_t epoch = 0;
};

} // namespace workflow_event

using WorkflowEvent = std::variant<workflow_event::RosterAfterAccept, workflow_event::RosterAfterRemoteAccept>;

/** What the session workflow tells the device (V057 2): events on the calls owner's queue. */
namespace workflow_report {

/** What the device shows about the calls may have changed (the ring / call chrome refreshes). */
struct Changed {};
/** A one-shot notice for the screen ("That call has ended"). */
struct Notice {
  std::string message;
};
/** A push woke us for a call: fetch the relay inbox now. */
struct InboxSyncFromWake {};

} // namespace workflow_report

using WorkflowReport =
    std::variant<workflow_report::Changed, workflow_report::Notice, workflow_report::InboxSyncFromWake>;

/** The hop-migrate workflow's own events (V050 group hop flows). */
namespace hop_migrate_event {

/** 2 s after a picked hop's fan-out: send it once more if the call is still on that hop. */
struct RefanOutPickedHop {
  std::string call_id;
  std::string encoded_attach;
  std::string local_identity;
  std::string hop_peer_id;
};
/** After a hop attach settled: drop the 1:1 path (and, for a self hop, fan out once more). */
struct ReleaseDirectAfterAttach {
  std::string call_id;
  uint64_t migrate_generation = 0;
  CallSfuAttachDetail fanout;
  bool self_hop = false;
};
/** #431 (b): look again whether the others are heard on the hop, and drop the 1:1 leg when they are. */
struct ReleaseDirectWhenHopHeard {
  std::string call_id;
  int64_t attached_ms = 0;
  int64_t deadline_ms = 0;
};
/** The guest's re-attach backoff ended: try again (`slow`: the steady pace past the quick attempts). */
struct GuestReattachRetry {
  bool slow = false;
};
/** A stored step's turn (a deferred step, or the circuit reach answered for a picked hop). */
struct Continue {
  uint64_t id = 0;
};
/** The media relay answered an attach (from its I/O thread); `id` names the attach awaiting it. */
struct RelayAttached {
  uint64_t id = 0;
  bool ok = false;
  int64_t a_up_bps = 0;
  std::string error;
};

} // namespace hop_migrate_event

using HopMigrateEvent =
    std::variant<hop_migrate_event::RefanOutPickedHop, hop_migrate_event::ReleaseDirectAfterAttach,
                 hop_migrate_event::ReleaseDirectWhenHopHeard,
                 hop_migrate_event::GuestReattachRetry, hop_migrate_event::Continue, hop_migrate_event::RelayAttached>;

/** V050 hop planning's own events. */
namespace planning_event {

/** A hop quote probe answered (from a worker / I/O); `round` names the invite's probe round. */
struct ProbeAnswered {
  std::string call_id;
  uint64_t round = 0;
  std::string hop_peer_id;
  bool ok = false;
  std::string error;
};

} // namespace planning_event

using PlanningEvent = std::variant<planning_event::ProbeAnswered>;

/** The topology controller's own events. */
namespace topology_event {

/** The media relay client's transport died (from the relay's thread); `watch` names that relay watch. */
struct RelayTransportLost {
  uint64_t watch = 0;
};
/** The attach-wait deadline armed as `armed` came. */
struct AttachWaitDeadline {
  std::string call_id;
  uint64_t armed = 0;
};
/** 2 s after announcing our publisher: once more, if the call is still on that hop / stream. */
struct ReannouncePublisher {
  std::string call_id;
  std::string encoded_attach;
  std::string local_identity;
  std::string hop_peer_id;
  uint32_t publisher_stream_id = 0;
};
/** No hop every participant reaches: refuse the guest (never on the receive path that found it). */
struct RefuseGuest {
  std::string call_id;
  std::string guest_identity;
};
/** A stored step's turn (a hop flow finished: its finish step runs as the next event). */
struct Continue {
  uint64_t id = 0;
};

} // namespace topology_event

using TopologyEvent = std::variant<topology_event::RelayTransportLost, topology_event::AttachWaitDeadline,
                                   topology_event::ReannouncePublisher, topology_event::RefuseGuest,
                                   topology_event::Continue, HopMigrateEvent>;

/** The 1:1 path's (CallMediaBridge) own events. */
/** What the 1:1 connect sequence (`CallMediaConnectCoordinator`) reports to itself. */
/** The device's doorway for inbound call media (V056). */
namespace doorway_event {

/** An inbound hello arrived (transport I/O). */
struct InboundHello {
  std::shared_ptr<CallMediaInboundReply> hello;
};
/** A media key landed: answer parked hellos that can be answered now. */
struct KeyAvailable {};
/** Re-check parked hellos (re-armed while any is parked). */
struct KeyPollTick {};

} // namespace doorway_event

using DoorwayEvent = std::variant<doorway_event::InboundHello, doorway_event::KeyAvailable, doorway_event::KeyPollTick>;

namespace connect_event {

/** Attempt `attempt` of sequence `seq` has its link (or failed to get one). */
struct LinkReady {
  uint64_t seq = 0;
  int attempt = 0;
  mesh::PeerReachCoordinator::ReachRoe reached;
};
/** Attempt `attempt`'s bundle open finished. */
struct AttemptDone {
  uint64_t seq = 0;
  int attempt = 0;
  CallMediaRoe connected;
};
/** Attempt `attempt` ran past its watchdog. */
struct WatchdogDue {
  uint64_t seq = 0;
  int attempt = 0;
};
/** The pause before attempt `attempt + 1` ended. */
struct RetryDue {
  uint64_t seq = 0;
  int attempt = 0;
};
/** Sequence `seq` ended: hand its result to the owner (never inside the step that ended it). */
struct Finished {
  uint64_t seq = 0;
  CallMediaRoe result;
};

} // namespace connect_event

using ConnectEvent = std::variant<connect_event::LinkReady, connect_event::AttemptDone, connect_event::WatchdogDue,
                                  connect_event::RetryDue, connect_event::Finished>;

namespace direct_event {

/** ~1 s connect-health tick while the 1:1 path connects / runs (re-armed by its handler). */
struct HealthTick {};
/** Circuit reservation renewal tick (re-armed by its handler). */
struct ReserveRenewTick {};
/** The next direct-upgrade attempt is due. */
struct UpgradeDue {};
/** The next relay-standby attempt is due. */
struct StandbyDue {};
/** Re-anchor a call that lost its last path. */
struct ReanchorDue {
  std::string call_id;
};
/** The grace for the peer's in-progress hello ended. */
struct RecoveryGraceOver {
  std::string call_id;
  std::string error;
};
/** A bundle connected (transport I/O). */
struct BundleConnected {
  std::string call_id;
  std::string label;
};
/** A bundle failed (transport I/O). */
struct BundleFailed {
  std::string call_id;
  std::string reason;
  CallMediaErr code = CallMediaErr::Generic;
};
/** k4: the call lost its last path (transport I/O). */
struct PathLost {
  std::string call_id;
};
/** k3: the transport moved the call to another path (transport I/O). */
struct PathChanged {
  std::string call_id;
  CallMediaLinkKind kind = CallMediaLinkKind::Unknown;
};
/** TX-only escalate tore the session down: start it again over a circuit. */
struct EscalateRestart {
  std::string call_id;
  std::string peer;
};
/** Start the offerer side of `call_id` (after the arming that scheduled it). */
struct StartOfferer {
  std::string call_id;
  std::string peer;
};
/** The call's media key landed (key exchange, or found by the key poll). */
struct MediaKeyReady {
  std::string call_id;
};
/** The deferred answerer's key poll: round `round` of key wait `wait` is due. */
struct KeyPollDue {
  std::string call_id;
  uint64_t wait = 0;
  int round = 0;
};
/** 1:1 frames arrive with no stream bound yet (transport I/O): try to bind it from what we know. */
struct RebindInboundStream {
  std::string call_id;
};
/** An event of the connect sequence (the bridge's child). */
struct ForConnect {
  ConnectEvent event;
};
/** A stored step's result arrived (a reach / upgrade / standby / migrate answer). */
struct StepReady {
  OwnerStepReady step;
};

} // namespace direct_event

using DirectPathEvent =
    std::variant<direct_event::HealthTick, direct_event::ReserveRenewTick, direct_event::UpgradeDue,
                 direct_event::StandbyDue, direct_event::ReanchorDue, direct_event::RecoveryGraceOver,
                 direct_event::BundleConnected, direct_event::BundleFailed,
                 direct_event::PathLost, direct_event::PathChanged, direct_event::EscalateRestart,
                 direct_event::StartOfferer, direct_event::MediaKeyReady, direct_event::KeyPollDue,
                 direct_event::RebindInboundStream, direct_event::ForConnect, direct_event::StepReady>;

/**
 * What the 1:1 path reports to the calls it serves (V055: events up, in its own terms). The calls'
 * owner moves the call's state on them; the path never writes it.
 */
namespace direct_report {

/** The path's planner moved (Reconnecting, DegradedTxOnly, …). */
struct Progress {
  std::string call_id;
  CallDirectPlannerPhase phase = CallDirectPlannerPhase::Idle;
};
/** Media flows over the 1:1 path (or over the hop that replaced it). */
struct Connected {
  std::string call_id;
};
/** The 1:1 stream ended because the call moves onto a hop not attached yet (nothing is live). */
struct HandedToHop {
  std::string call_id;
};
/** No 1:1 path could be kept: the call stays open, failed. */
struct ConnectFailed {
  std::string call_id;
};
/** H013: we placed this 1:1 call and stopped trying to reach `peer_identity` — tell it. */
struct GaveUpReaching {
  std::string call_id;
  std::string peer_identity;
};
/** A connection for this failed call came up (the peer's retry). */
struct PeerReconnected {
  std::string call_id;
};
/** The path started trying media for the call (connect, key wait or failure). */
struct MediaAttempted {
  std::string call_id;
};
/** The answerer's media waits for the call's key. */
struct MediaDeferred {
  std::string call_id;
};
/** The key the answerer waited for landed: its 1:1 start can go ahead with `peer_identity`. */
struct MediaKeyReady {
  std::string call_id;
  std::string peer_identity;
};

/** What the device shows about the call may have changed (the ring / call chrome refreshes). */
struct Changed {
  std::string call_id;
};
/** A media error for the screen to show once. */
struct MediaError {
  std::string call_id;
  std::string message;
};
/** The offerer's connect retries: send the call's current media key to `peer_identity` again. */
struct MediaKeyResendWanted {
  std::string call_id;
  std::string peer_identity;
};
/** The answerer waits for the key: fetch the relay inbox now. */
struct InboxSyncWanted {
  std::string call_id;
};
/**
 * An inbound hello from `identity` (account) / `peer_id` (mesh) was accepted: B30, it may stand in for
 * a CallAccept the relay has not delivered yet.
 */
struct InboundHello {
  std::string call_id;
  std::string identity;
  std::string peer_id;
};

} // namespace direct_report

using DirectReport =
    std::variant<direct_report::Progress, direct_report::Connected, direct_report::HandedToHop,
                 direct_report::ConnectFailed, direct_report::GaveUpReaching, direct_report::PeerReconnected,
                 direct_report::MediaAttempted, direct_report::MediaDeferred, direct_report::MediaKeyReady,
                 direct_report::Changed, direct_report::MediaError, direct_report::MediaKeyResendWanted,
                 direct_report::InboxSyncWanted, direct_report::InboundHello>;

/** What a call's group (hop) path tells the device (V057 2): events on the calls owner's queue. */
namespace hop_report {

/** The path's planner moved. */
struct Progress {
  std::string call_id;
  CallHopPlannerPhase phase = CallHopPlannerPhase::Idle;
};
/** What the device shows about the call may have changed (the ring / call chrome refreshes). */
struct Changed {
  std::string call_id;
};
/** A media error for the screen to show once. */
struct MediaError {
  std::string call_id;
  std::string message;
};
/** The call's media activity line ("Switching media path…"); empty clears it. */
struct Activity {
  std::string call_id;
  std::string text;
};
/** The path started trying media for the call. */
struct MediaAttempted {
  std::string call_id;
};
/** A guest waits for CallSfuAttach: fetch the relay inbox now. */
struct InboxSyncWanted {
  std::string call_id;
};
/** The path cannot keep the call (no hop everyone reaches, an owner gone): leave it. */
struct LeaveWanted {
  std::string call_id;
};

} // namespace hop_report

using HopReport = std::variant<hop_report::Progress, hop_report::Changed, hop_report::MediaError, hop_report::Activity,
                               hop_report::MediaAttempted, hop_report::InboxSyncWanted, hop_report::LeaveWanted>;

namespace session_event {

/** An event of `call_id`'s 1:1 path `serial` (a path built since, or the call gone, drops it). */
struct ForDirectPath {
  std::string call_id;
  uint64_t serial = 0;
  DirectPathEvent event;
};
/** A report of `call_id`'s 1:1 path `serial` (dropped as above). */
struct FromDirectPath {
  std::string call_id;
  uint64_t serial = 0;
  DirectReport report;
};
/** An event of `call_id`'s group path `serial` (a path built since, or the call gone, drops it). */
struct ForHopPath {
  std::string call_id;
  uint64_t serial = 0;
  TopologyEvent event;
};
/** A report of the group path. */
struct FromHopPath {
  HopReport report;
};
/** An event of the device's hop service (planning's probe answers). */
struct ForHopService {
  PlanningEvent event;
};
/** What the session workflow reported. */
struct FromWorkflow {
  WorkflowReport report;
};
/** An event of the inbound doorway `generation` (one built since drops it). */
struct ForDoorway {
  uint64_t generation = 0;
  DoorwayEvent event;
};

} // namespace session_event

using SessionEvent = std::variant<session_event::AnswererKickRetry, session_event::MediaRestart, WorkflowEvent,
                                  session_event::ForHopPath, session_event::ForDirectPath,
                                  session_event::FromDirectPath, session_event::FromHopPath, session_event::ForDoorway,
                                  session_event::ForHopService, session_event::FromWorkflow>;

} // namespace pbr
