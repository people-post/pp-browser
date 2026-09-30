#pragma once

#include "domain/messaging/CallTypes.h"

#include <cstdint>
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
};
/** After a peer's accept started media: roster fan-out, prefetch the peer's reach. */
struct RosterAfterRemoteAccept {
  std::string call_id;
  std::string peer;
  std::string local_identity;
};

} // namespace workflow_event

using WorkflowEvent = std::variant<workflow_event::RosterAfterAccept, workflow_event::RosterAfterRemoteAccept>;

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

using SessionEvent =
    std::variant<session_event::AnswererKickRetry, session_event::MediaRestart, WorkflowEvent, TopologyEvent>;

} // namespace pbr
