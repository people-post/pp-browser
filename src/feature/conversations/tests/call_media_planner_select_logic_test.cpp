#include "domain/messaging/CallMediaPlannerSelectLogic.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(CallMediaPlannerSelectLogicTest, ArmHopOnlyWhenNGe3) {
  EXPECT_FALSE(ShouldArmHopPlanner(1));
  EXPECT_FALSE(ShouldArmHopPlanner(2));
  EXPECT_TRUE(ShouldArmHopPlanner(3));
  EXPECT_TRUE(ShouldArmHopPlanner(8));
}

// V050: a 1:1 whose other invitees still ring is not a group yet — only joins move topology up.
TEST(CallMediaPlannerSelectLogicTest, ExpectGroupSfuOnlyFromJoined) {
  CallExpectGroupSfuInput in;
  in.joined_count = 2;
  EXPECT_FALSE(ShouldExpectGroupSfuMigration(in));
  in.joined_count = 3;
  EXPECT_TRUE(ShouldExpectGroupSfuMigration(in));
}

TEST(CallMediaPlannerSelectLogicTest, ExpectGroupSfuFromHint) {
  CallExpectGroupSfuInput in;
  in.joined_count = 2;
  in.has_sfu_hint = true;
  EXPECT_TRUE(ShouldExpectGroupSfuMigration(in));
}

TEST(CallMediaPlannerSelectLogicTest, NoExpectOnPlainOneToOne) {
  CallExpectGroupSfuInput in;
  in.joined_count = 2;
  EXPECT_FALSE(ShouldExpectGroupSfuMigration(in));
}

TEST(CallMediaPlannerSelectLogicTest, RelayCapNudgeSkippedForOneToOne) {
  CallRelayCapNudgeInput in;
  in.media_relay_newly_true = true;
  in.joined_count = 2;
  EXPECT_FALSE(ShouldNudgeSoftMigrateOnRelayCap(in));
}

TEST(CallMediaPlannerSelectLogicTest, RelayCapNudgeWhenNGe3) {
  CallRelayCapNudgeInput in;
  in.media_relay_newly_true = true;
  in.joined_count = 3;
  EXPECT_TRUE(ShouldNudgeSoftMigrateOnRelayCap(in));
}

TEST(CallMediaPlannerSelectLogicTest, RelayCapNudgeWhenAttachWait) {
  CallRelayCapNudgeInput in;
  in.media_relay_newly_true = true;
  in.joined_count = 2;
  in.sfu_attach_wait_active = true;
  EXPECT_TRUE(ShouldNudgeSoftMigrateOnRelayCap(in));
}

TEST(CallMediaPlannerSelectLogicTest, RelayCapNudgeSkippedWhenAlreadyOnSfu) {
  CallRelayCapNudgeInput in;
  in.media_relay_newly_true = true;
  in.joined_count = 3;
  in.already_on_sfu_for_call = true;
  EXPECT_FALSE(ShouldNudgeSoftMigrateOnRelayCap(in));
}

TEST(CallMediaPlannerSelectLogicTest, RelayCapNudgeSkippedWhenAttachedStable) {
  CallRelayCapNudgeInput in;
  in.media_relay_newly_true = true;
  in.joined_count = 3;
  in.sfu_attached = true;
  in.sfu_attach_wait_active = false;
  EXPECT_FALSE(ShouldNudgeSoftMigrateOnRelayCap(in));
}

} // namespace
} // namespace pbr
