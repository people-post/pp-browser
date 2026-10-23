#include "lib/calls/CallSummaryLogic.h"

#include <algorithm>

namespace pbr {

std::optional<CallSummaryPlan> PlanCallSummary(const CallSession& session,
                                               const std::vector<CallParticipant>& participants,
                                               const std::string& local_identity, const LiveCallEndReason reason,
                                               const int64_t now_ms) {
  if (reason == LiveCallEndReason::Merged) {
    return std::nullopt;  // gave way to the call both sides are now in: that one is the history
  }
  const bool group = session.origin_group_id.has_value() && !session.origin_group_id->empty();
  const bool started = session.initiator_identity == local_identity;
  if (!group && !started) {
    return std::nullopt;  // 1:1: the starter reports the call; the other side reads its summary
  }
  CallSummaryPlan plan;
  CallSummaryDetail& summary = plan.detail;
  summary.call_id = session.call_id;
  summary.media_mode = session.media_mode;
  summary.group = group;
  summary.started = started;

  std::optional<int64_t> self_joined;
  std::optional<int64_t> other_joined;
  for (const CallParticipant& participant : participants) {
    if (!participant.joined_at || *participant.joined_at <= 0) {
      continue;
    }
    if (!group && participant.identity == session.initiator_identity) {
      continue;
    }
    auto& slot = participant.identity == local_identity ? self_joined : other_joined;
    slot = slot ? std::min(*slot, *participant.joined_at) : *participant.joined_at;
  }
  std::optional<int64_t> answered_at;
  if (!group) {
    answered_at = other_joined;
  } else if (self_joined && other_joined) {
    answered_at = std::max(*self_joined, *other_joined);
  }
  if (reason == LiveCallEndReason::Orphaned) {
    answered_at.reset();  // ended by a restart, maybe hours later: no talk time to report
  }
  summary.outcome = SummaryOutcome(answered_at.has_value(), reason);
  if (group && !answered_at && !started) {
    summary.outcome = reason == LiveCallEndReason::Declined ? CallOutcome::Declined : CallOutcome::Missed;
  }
  if (answered_at) {
    summary.duration_ms = std::max<int64_t>(0, session.ended_at.value_or(now_ms) - *answered_at);
  }
  plan.local_only = group || reason == LiveCallEndReason::Shutdown;
  return plan;
}

} // namespace pbr
