#pragma once

#include "lib/calls/CallTypes.h"
#include "lib/calls/LiveCall.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/** A call's history entry as this device writes it, and where it goes. */
struct CallSummaryPlan {
  CallSummaryDetail detail;
  /** A group has no one view of a call: its summary stays here. 1:1 goes to both sides unless the app is going down. */
  bool local_only = false;
};

/**
 * The summary `local_identity` writes for an ended call; none when it writes none (a merged call
 * gave way to the one both sides are in; a 1:1 call is reported by its starter). 1:1: answered when
 * the other side joined. Group: each device notes the call as it went for its own user — answered
 * when we were in it with someone else, talk time from when both were; an invitee that never
 * joined declined or missed it. A call ended by a restart (Orphaned) reports no talk time.
 */
std::optional<CallSummaryPlan> PlanCallSummary(const CallSession& session,
                                               const std::vector<CallParticipant>& participants,
                                               const std::string& local_identity, LiveCallEndReason reason,
                                               int64_t now_ms);

} // namespace pbr
