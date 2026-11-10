#include "feature/presenters/CallStepScroll.h"
#include "feature/presenters/CallStoryList.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

constexpr int kRow = call_page_dp::kStepRow;

CallStoryListInput Answerer(const int stage) {
  CallStoryListInput in;
  in.view.outgoing = false;
  in.view.stage = stage;
  in.story_active = true;
  in.caller = "Wang";
  return in;
}

TEST(CallStoryListTest, AnAnsweredOneToOneVoiceCallOpensWithThreeDoneRowsAndTheSparkComesTowardsUs) {
  const CallStoryList plan = PlanCallStoryList(Answerer(CallConnectStory::kStageAnswered));
  ASSERT_EQ(plan.lead.size(), 3u);
  EXPECT_EQ(plan.lead[0].key, "call.ring.step.received_voice");
  EXPECT_EQ(plan.lead[2].text, "Wang");
  for (const CallRingStep& step : plan.lead) {
    EXPECT_EQ(step.state, CallRingStep::State::Done);
  }
  EXPECT_TRUE(plan.inbound);
}

TEST(CallStoryListTest, TheLeadRowsUseTheKindTheCallRangWith) {
  CallStoryListInput in = Answerer(CallConnectStory::kStageAnswered);
  in.media = CallRingMedia::Video;
  EXPECT_EQ(PlanCallStoryList(in).lead[0].key, "call.ring.step.received_video");
}

TEST(CallStoryListTest, AnOutgoingCallHasNoLeadRowsAndTheSparkRunsAwayFromUs) {
  CallStoryListInput in = Answerer(CallConnectStory::kStageCalling);
  in.view.outgoing = true;
  const CallStoryList plan = PlanCallStoryList(in);
  EXPECT_TRUE(plan.lead.empty());
  EXPECT_FALSE(plan.inbound);
  EXPECT_EQ(plan.reached_row, 1);
}

TEST(CallStoryListTest, AGroupHasNoLeadRows) {
  CallStoryListInput in = Answerer(0);
  in.group = true;
  const CallStoryList plan = PlanCallStoryList(in);
  EXPECT_TRUE(plan.lead.empty());
  EXPECT_EQ(plan.scroll_rows, 0);
  EXPECT_EQ(plan.clip_dp, 0);
}

TEST(CallStoryListTest, TheVideoCardHasNoLeadRowsButItsSparkStillComesTowardsUs) {
  CallStoryListInput in = Answerer(CallConnectStory::kStageAnswered);
  in.video_stage = true;
  const CallStoryList plan = PlanCallStoryList(in);
  EXPECT_TRUE(plan.lead.empty());
  EXPECT_TRUE(plan.inbound);
}

TEST(CallStoryListTest, TheReachedRowAtEachStageCountsTheLeadRows) {
  EXPECT_EQ(PlanCallStoryList(Answerer(CallConnectStory::kStageAnswered)).reached_row, 4);
  EXPECT_EQ(PlanCallStoryList(Answerer(CallConnectStory::kStageMatching)).reached_row, 5);
  EXPECT_EQ(PlanCallStoryList(Answerer(CallConnectStory::kStageSecuring)).reached_row, 6);
  EXPECT_EQ(PlanCallStoryList(Answerer(CallConnectStory::kStageDone)).reached_row, 7) << "all seven done";
}

TEST(CallStoryListTest, NothingIsPlannedOnceTheStoryIsNotActive) {
  CallStoryListInput in = Answerer(CallConnectStory::kStageDone);
  in.story_active = false;
  in.page_height_dp = 500;
  const CallStoryList plan = PlanCallStoryList(in);
  EXPECT_TRUE(plan.lead.empty());
  EXPECT_EQ(plan.scroll_rows, 0);
  EXPECT_EQ(plan.clip_dp, 0) << "a connected call's key must not follow the page height";
}

TEST(CallStoryListTest, AListThatFitsIsNotClippedAndAListThatDoesNotIsClippedToTheRowsThatFit) {
  using namespace call_page_dp;
  CallStoryListInput in = Answerer(CallConnectStory::kStageDone); // row 7 reached
  in.page_height_dp = kStepsStart + kControlsReserve + 7 * kRow;
  EXPECT_EQ(PlanCallStoryList(in).scroll_rows, 0);
  EXPECT_EQ(PlanCallStoryList(in).clip_dp, 0) << "no fixed clip height while nothing moves";
  in.page_height_dp = kStepsStart + kControlsReserve + 5 * kRow;
  const CallStoryList small = PlanCallStoryList(in);
  EXPECT_EQ(small.scroll_rows, 2);
  EXPECT_EQ(small.clip_dp, 5 * kRow - 12) << "exactly the five visible rows";
}

TEST(CallStoryListTest, AHintUnderTheListTakesRoomFromIt) {
  using namespace call_page_dp;
  CallStoryListInput in = Answerer(CallConnectStory::kStageDone);
  in.page_height_dp = kStepsStart + kControlsReserve + 5 * kRow + kHintLine;
  EXPECT_EQ(PlanCallStoryList(in).scroll_rows, 2);
  in.hint_shown = true;
  EXPECT_EQ(PlanCallStoryList(in).scroll_rows, 2) << "the hint line fits its own room";
  in.page_height_dp -= 1;
  EXPECT_EQ(PlanCallStoryList(in).scroll_rows, 3) << "one row less visible";
}

} // namespace
} // namespace pbr
