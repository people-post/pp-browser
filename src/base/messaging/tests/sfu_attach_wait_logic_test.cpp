#include "base/messaging/SfuAttachWaitLogic.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(SfuAttachWaitLogicTest, IdleWhenNoWait) {
  SfuAttachWaitPollInput in;
  in.wait_active = false;
  EXPECT_EQ(PollSfuAttachWait(in), SfuAttachWaitPollResult::Idle);
}

TEST(SfuAttachWaitLogicTest, ClearWhenAttached) {
  SfuAttachWaitPollInput in;
  in.wait_active = true;
  in.sfu_attached_for_call = true;
  in.now_ms = 100;
  in.deadline_ms = 50;
  EXPECT_EQ(PollSfuAttachWait(in), SfuAttachWaitPollResult::ClearAttached);
}

TEST(SfuAttachWaitLogicTest, ClearAsP2pWhenJoinedUnderThree) {
  SfuAttachWaitPollInput in;
  in.wait_active = true;
  in.joined_count = 2;
  in.media_active_mesh_for_call = true;
  in.now_ms = 100;
  in.deadline_ms = 50;
  EXPECT_EQ(PollSfuAttachWait(in), SfuAttachWaitPollResult::ClearAsP2p);
}

TEST(SfuAttachWaitLogicTest, NoTimeoutWhileMigrateInFlight) {
  SfuAttachWaitPollInput in;
  in.wait_active = true;
  in.joined_count = 3;
  in.soft_migrate_in_flight = true;
  in.now_ms = 100'000;
  in.deadline_ms = 1;
  EXPECT_EQ(PollSfuAttachWait(in), SfuAttachWaitPollResult::Waiting);
}

TEST(SfuAttachWaitLogicTest, TimeoutLeaveWhenPastDeadline) {
  SfuAttachWaitPollInput in;
  in.wait_active = true;
  in.joined_count = 3;
  in.soft_migrate_in_flight = false;
  in.now_ms = 100'000;
  in.deadline_ms = 1;
  EXPECT_EQ(PollSfuAttachWait(in), SfuAttachWaitPollResult::TimeoutLeave);
}

TEST(SfuAttachWaitLogicTest, WaitingBeforeDeadline) {
  SfuAttachWaitPollInput in;
  in.wait_active = true;
  in.joined_count = 3;
  in.now_ms = 10;
  in.deadline_ms = 100;
  EXPECT_EQ(PollSfuAttachWait(in), SfuAttachWaitPollResult::Waiting);
}

} // namespace
} // namespace pbr
