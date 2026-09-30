#pragma once

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

using SessionEvent = std::variant<session_event::AnswererKickRetry, session_event::MediaRestart, WorkflowEvent>;

} // namespace pbr
