#include "feature/calls/CallMediaPlannerSelectLogic.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(CallMediaPlannerSelectLogicTest, EffectiveNPrefersActiveRoster) {
  EXPECT_EQ(EffectiveMediaPlannerN(2, 2), 2u);
  EXPECT_EQ(EffectiveMediaPlannerN(2, 3), 3u);
  EXPECT_EQ(EffectiveMediaPlannerN(3, 2), 3u);
}

TEST(CallMediaPlannerSelectLogicTest, ArmHopOnlyWhenNGe3) {
  EXPECT_FALSE(ShouldArmHopPlanner(1));
  EXPECT_FALSE(ShouldArmHopPlanner(2));
  EXPECT_TRUE(ShouldArmHopPlanner(3));
  EXPECT_TRUE(ShouldArmHopPlanner(8));
}

TEST(CallMediaPlannerSelectLogicTest, CountActiveParticipants) {
  std::vector<CallParticipant> rows(4);
  rows[0].state = CallParticipantState::Joined;
  rows[1].state = CallParticipantState::Ringing;
  rows[2].state = CallParticipantState::Invited;
  rows[3].state = CallParticipantState::Left;
  EXPECT_EQ(CountMediaPlannerActiveParticipants(rows), 3u);
}

TEST(CallMediaPlannerSelectLogicTest, ExpectGroupSfuFromActiveRoster) {
  CallExpectGroupSfuInput in;
  in.joined_count = 2;
  in.active_roster_count = 3;
  EXPECT_TRUE(ShouldExpectGroupSfuMigration(in));
}

TEST(CallMediaPlannerSelectLogicTest, ExpectGroupSfuFromHint) {
  CallExpectGroupSfuInput in;
  in.joined_count = 2;
  in.active_roster_count = 2;
  in.has_sfu_hint = true;
  EXPECT_TRUE(ShouldExpectGroupSfuMigration(in));
}

TEST(CallMediaPlannerSelectLogicTest, NoExpectOnPlainOneToOne) {
  CallExpectGroupSfuInput in;
  in.joined_count = 2;
  in.active_roster_count = 2;
  EXPECT_FALSE(ShouldExpectGroupSfuMigration(in));
}

TEST(CallMediaPlannerSelectLogicTest, RelayCapNudgeSkippedForOneToOne) {
  CallRelayCapNudgeInput in;
  in.media_relay_newly_true = true;
  in.effective_n = 2;
  EXPECT_FALSE(ShouldNudgeSoftMigrateOnRelayCap(in));
}

TEST(CallMediaPlannerSelectLogicTest, RelayCapNudgeWhenNGe3) {
  CallRelayCapNudgeInput in;
  in.media_relay_newly_true = true;
  in.effective_n = 3;
  EXPECT_TRUE(ShouldNudgeSoftMigrateOnRelayCap(in));
}

TEST(CallMediaPlannerSelectLogicTest, RelayCapNudgeWhenAttachWait) {
  CallRelayCapNudgeInput in;
  in.media_relay_newly_true = true;
  in.effective_n = 2;
  in.sfu_attach_wait_active = true;
  EXPECT_TRUE(ShouldNudgeSoftMigrateOnRelayCap(in));
}

TEST(CallMediaPlannerSelectLogicTest, RelayCapNudgeSkippedWhenAlreadyOnSfu) {
  CallRelayCapNudgeInput in;
  in.media_relay_newly_true = true;
  in.effective_n = 3;
  in.already_on_sfu_for_call = true;
  EXPECT_FALSE(ShouldNudgeSoftMigrateOnRelayCap(in));
}

TEST(CallMediaPlannerSelectLogicTest, RelayCapNudgeSkippedWhenAttachedStable) {
  CallRelayCapNudgeInput in;
  in.media_relay_newly_true = true;
  in.effective_n = 3;
  in.sfu_attached = true;
  in.sfu_attach_wait_active = false;
  EXPECT_FALSE(ShouldNudgeSoftMigrateOnRelayCap(in));
}

} // namespace
} // namespace pbr
