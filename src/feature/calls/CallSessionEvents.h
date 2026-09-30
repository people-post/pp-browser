#pragma once

#include "domain/mesh/l4/call_media/ICallMediaTransport.h"
#include "domain/mesh/reach/PeerReachCoordinator.h"
#include "domain/messaging/CallTypes.h"
#include "feature/calls/CallMediaInboundReply.h"
#include "foundation/runtime/OwnerSteps.h"

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include "common/PbrCompat.h"

namespace pbr {

/** What the call session manager's subtree reports to itself through the calls owner's queue. */
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

/** A waiting step's result arrived (the accept's circuit park). */
struct Continue {
  OwnerStepReady step;
};

} // namespace workflow_event

using WorkflowEvent = std::variant<workflow_event::RosterAfterAccept, workflow_event::RosterAfterRemoteAccept,
                                   workflow_event::Continue>;

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
/** The guest's re-attach backoff ended: try again. */
struct GuestReattachRetry {};
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
                                   topology_event::Continue, HopMigrateEvent, PlanningEvent>;

/** The 1:1 path's (CallMediaBridge) own events. */
/** What the 1:1 connect sequence (`CallMediaConnectCoordinator`) reports to itself. */
namespace connect_event {

/** An inbound hello arrived (transport I/O). */
struct InboundHello {
  std::shared_ptr<CallMediaInboundReply> hello;
};
/** A media key landed: answer parked hellos that can be answered now. */
struct KeyAvailable {};
/** Re-check parked hellos (re-armed while any is parked). */
struct KeyPollTick {};
/** Attempt `attempt` of sequence `seq` has its link (or failed to get one). */
struct LinkReady {
  uint64_t seq = 0;
  int attempt = 0;
  Roe<PeerReachResult> reached;
};
/** Attempt `attempt`'s bundle open finished. */
struct AttemptDone {
  uint64_t seq = 0;
  int attempt = 0;
  Roe<void> connected;
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
  Roe<void> result;
};

} // namespace connect_event

using ConnectEvent =
    std::variant<connect_event::InboundHello, connect_event::KeyAvailable, connect_event::KeyPollTick,
                 connect_event::LinkReady, connect_event::AttemptDone, connect_event::WatchdogDue,
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

namespace session_event {

/** An event of the 1:1 path; `generation` names that path (one built since drops it). */
struct ForDirectPath {
  uint64_t generation = 0;
  DirectPathEvent event;
};

} // namespace session_event

using SessionEvent = std::variant<session_event::AnswererKickRetry, session_event::MediaRestart, WorkflowEvent,
                                  TopologyEvent, session_event::ForDirectPath>;

} // namespace pbr
