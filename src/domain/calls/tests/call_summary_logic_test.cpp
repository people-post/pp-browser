// The history entry a device writes when a call ends.
#include "domain/calls/CallSummaryLogic.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

CallParticipant Joined(const std::string& identity, std::optional<int64_t> at) {
  CallParticipant p;
  p.identity = identity;
  p.state = CallParticipantState::Joined;
  p.joined_at = at;
  return p;
}

CallSession Session(const std::string& initiator, bool group) {
  CallSession s;
  s.call_id = "call:s";
  s.initiator_identity = initiator;
  s.ended_at = 10'000;
  if (group) {
    s.origin_group_id = "g";
  }
  return s;
}

TEST(CallSummaryLogicTest, OneToOneIsTheStartersToReport) {
  const auto participants = std::vector<CallParticipant>{Joined("me", 1'000), Joined("peer", 4'000)};
  EXPECT_FALSE(PlanCallSummary(Session("peer", false), participants, "me", LiveCallEndReason::LocalLeave, 0));
  auto plan = PlanCallSummary(Session("me", false), participants, "me", LiveCallEndReason::LocalLeave, 0);
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->detail.outcome, CallOutcome::Completed);
  EXPECT_EQ(plan->detail.duration_ms, std::optional<int64_t>(6'000)) << "from when the other side joined";
  EXPECT_FALSE(plan->local_only) << "goes to both sides";
  EXPECT_TRUE(PlanCallSummary(Session("me", false), participants, "me", LiveCallEndReason::Shutdown, 0)->local_only);
  EXPECT_FALSE(PlanCallSummary(Session("me", false), participants, "me", LiveCallEndReason::Merged, 0));

  auto missed = PlanCallSummary(Session("me", false), {Joined("me", 1'000)}, "me", LiveCallEndReason::Unanswered, 0);
  EXPECT_EQ(missed->detail.outcome, CallOutcome::Missed);
  EXPECT_FALSE(missed->detail.duration_ms);
  auto restarted = PlanCallSummary(Session("me", false), participants, "me", LiveCallEndReason::Orphaned, 0);
  EXPECT_FALSE(restarted->detail.duration_ms) << "no talk time after a restart";
}

TEST(CallSummaryLogicTest, AGroupNotesTheCallAsItWentHere) {
  const auto participants =
      std::vector<CallParticipant>{Joined("owner", 1'000), Joined("me", 5'000), Joined("other", 3'000)};
  auto plan = PlanCallSummary(Session("owner", true), participants, "me", LiveCallEndReason::LocalLeave, 0);
  ASSERT_TRUE(plan);
  EXPECT_TRUE(plan->local_only);
  EXPECT_EQ(plan->detail.duration_ms, std::optional<int64_t>(5'000)) << "from when both of us were in";
  auto never = PlanCallSummary(Session("owner", true), {Joined("owner", 1'000)}, "me", LiveCallEndReason::Expired, 0);
  EXPECT_EQ(never->detail.outcome, CallOutcome::Missed);
  auto declined =
      PlanCallSummary(Session("owner", true), {Joined("owner", 1'000)}, "me", LiveCallEndReason::Declined, 0);
  EXPECT_EQ(declined->detail.outcome, CallOutcome::Declined);
}

} // namespace
} // namespace pbr
