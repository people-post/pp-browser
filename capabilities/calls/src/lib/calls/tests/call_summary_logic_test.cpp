// The history entry a device writes when a call ends.
#include "lib/calls/CallSummaryLogic.h"

#include "lib/calls/CallControlCodec.h"

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

TEST(CallSummaryLogicTest, ACallAnsweredOnAnotherOfOurDevicesLeavesNoHistoryHere) {
  // M021 d5: the device that took it (and the caller) keep the history; a sibling that only rang writes no
  // "missed call" — in a group either.
  EXPECT_FALSE(PlanCallSummary(Session("owner", true), {Joined("owner", 1'000)}, "me",
                               LiveCallEndReason::AnsweredElsewhere, 0));
  EXPECT_FALSE(PlanCallSummary(Session("me", false), {Joined("me", 1'000)}, "me", LiveCallEndReason::AnsweredElsewhere,
                               0));
}

TEST(CallSummaryLogicTest, AGroupRecordHasOneFixedIdPerCall) {
  EXPECT_EQ(GroupCallSummaryMessageId("call:s"), "call-summary:call:s");
}

namespace {
CallSummaryDetail Stay(int64_t started_at, int64_t duration_ms) {
  CallSummaryDetail stay;
  stay.group = true;
  stay.talk_started_at = started_at;
  stay.duration_ms = duration_ms;
  return stay;
}
} // namespace

TEST(CallSummaryLogicTest, ALaterLeaveRunsFromTheFirstJoinToTheLastLeave) {
  const CallSummaryDetail first = Stay(1'000, 4'000);   // 1s to 5s
  const CallSummaryDetail second = Stay(8'000, 2'000);  // 8s to 10s, its own stamp
  EXPECT_EQ(MergeGroupSummary(std::nullopt, first, SummaryWrite::Leave)->duration_ms, std::optional<int64_t>(4'000));
  const auto merged = MergeGroupSummary(first, second, SummaryWrite::Leave);
  ASSERT_TRUE(merged);
  EXPECT_EQ(merged->duration_ms, std::optional<int64_t>(9'000));
  EXPECT_EQ(merged->talk_started_at, std::optional<int64_t>(1'000));
  CallSummaryDetail none;
  EXPECT_FALSE(MergeGroupSummary(first, none, SummaryWrite::Leave)) << "a segment without talk time changes nothing";
}

TEST(CallSummaryLogicTest, WritingOneStayTwiceDoesNotGrowTheRecord) {
  const CallSummaryDetail first = Stay(1'000, 4'000);
  const CallSummaryDetail second = Stay(8'000, 2'000);
  const auto once = MergeGroupSummary(first, second, SummaryWrite::Leave);
  const auto twice = MergeGroupSummary(once, second, SummaryWrite::EndWhileIn);
  EXPECT_EQ(twice->duration_ms, once->duration_ms);
}

// A roster from before the rejoin put the first stay's stamp back: still first join to last leave.
TEST(CallSummaryLogicTest, AStaleStampDoesNotCountTheFirstStayTwice) {
  const CallSummaryDetail first = Stay(1'000, 4'000);
  const CallSummaryDetail stale = Stay(1'000, 9'000);
  EXPECT_EQ(MergeGroupSummary(first, stale, SummaryWrite::Leave)->duration_ms, std::optional<int64_t>(9'000));
}

TEST(CallSummaryLogicTest, ARecordWithoutTalkTimeTakesTheLaterWrite) {
  CallSummaryDetail missed;
  missed.group = true;
  missed.outcome = CallOutcome::Missed;
  CallSummaryDetail joined_alone;
  joined_alone.group = true;
  joined_alone.outcome = CallOutcome::Completed;
  EXPECT_EQ(MergeGroupSummary(missed, joined_alone, SummaryWrite::Leave)->outcome, CallOutcome::Completed);
}

TEST(CallSummaryLogicTest, TheEndOfACallThisDeviceLeftNeverReplacesItsRecord) {
  const CallSummaryDetail left = Stay(1'000, 4'000);
  CallSummaryDetail ended = left;
  ended.duration_ms = 9'000;  // counted to the call's end, after this device left
  EXPECT_FALSE(MergeGroupSummary(left, ended, SummaryWrite::Final));
  EXPECT_TRUE(MergeGroupSummary(std::nullopt, ended, SummaryWrite::Final)) << "a call never left writes at its end";
}

TEST(CallSummaryLogicTest, TheEndOfACallThisDeviceWasBackInRunsToTheEnd) {
  const CallSummaryDetail left = Stay(1'000, 4'000);
  const CallSummaryDetail ended = Stay(8'000, 3'000);  // back in after the leave, and in it when the call ended
  EXPECT_EQ(MergeGroupSummary(left, ended, SummaryWrite::EndWhileIn)->duration_ms, std::optional<int64_t>(10'000));
  CallSummaryDetail no_talk = left;
  no_talk.duration_ms.reset();
  EXPECT_FALSE(MergeGroupSummary(left, no_talk, SummaryWrite::EndWhileIn));
}

TEST(CallControlCodecTest, AnsweredElsewhereNamesTheCallAndHowItWasAnswered) {
  auto detail = CallControlCodec::EncodeAnsweredElsewhere("call:1", false);
  ASSERT_TRUE(detail);
  auto decoded = CallControlCodec::DecodeAnsweredElsewhere(*detail);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->first, "call:1");
  EXPECT_FALSE(decoded->second) << "declined there";
  EXPECT_FALSE(CallControlCodec::DecodeAnsweredElsewhere("{}"));
  EXPECT_TRUE(CallControlCodec::TravelsOnLane(CallControlType::CallAnsweredElsewhere));
  EXPECT_EQ(CallControlTypeFromWire(CallControlTypeToWire(CallControlType::CallAnsweredElsewhere)),
            CallControlType::CallAnsweredElsewhere);
}

} // namespace
} // namespace pbr
