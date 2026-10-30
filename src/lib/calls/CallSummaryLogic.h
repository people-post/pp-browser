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

/** The id of a group call's record in the thread: one per call on this device, rewritten as it goes. */
std::string GroupCallSummaryMessageId(const std::string& call_id);

/** Why a summary is written: the call ended for this device (Final), or this device left a live call (Leave). */
/**
 * Final: the call ended (or a restart cleaned it up) after this device had left it. EndWhileIn: it ended
 * while this device was in it. Leave: this device left a call that goes on for the others.
 */
enum class SummaryWrite { Final, EndWhileIn, Leave };

/**
 * What a group call's record becomes when `next` is written over `existing` (the record already in the
 * thread, if any); nullopt = leave it as it is. A Final write never replaces a record. An EndWhileIn write
 * replaces it only with longer talk time (back in the call after a leave). A Leave write replaces it: the
 * talk time runs from this device's first join to its last leave (leave, rejoin, leave).
 */
std::optional<CallSummaryDetail> MergeGroupSummary(const std::optional<CallSummaryDetail>& existing,
                                                   const CallSummaryDetail& next, SummaryWrite mode);

} // namespace pbr
