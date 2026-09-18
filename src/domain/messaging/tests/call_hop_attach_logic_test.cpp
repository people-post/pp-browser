#include "domain/messaging/CallHopAttachLogic.h"
#include "domain/messaging/CallTypes.h"

#include <gtest/gtest.h>

using namespace pbr;

TEST(CallHopAttachLogicTest, FanoutClearsConsumedQuoteId) {
  CallSfuAttachDetail after;
  after.call_id = "call:1";
  after.hop_peer_id = "hop";
  after.quote_id = "consumed-quote";
  after.multiaddrs = {"/ip4/1.2.3.4/tcp/1"};

  const CallSfuAttachDetail fanout = BuildSfuAttachFanout(after);
  EXPECT_EQ(fanout.call_id, "call:1");
  EXPECT_EQ(fanout.hop_peer_id, "hop");
  EXPECT_TRUE(fanout.quote_id.empty());
  EXPECT_EQ(fanout.multiaddrs.size(), 1u);
}

TEST(CallHopAttachLogicTest, PublisherStreamIdStable) {
  const uint32_t a = PublisherStreamIdForIdentity("account:alice");
  const uint32_t b = PublisherStreamIdForIdentity("account:alice");
  EXPECT_EQ(a, b);
  EXPECT_NE(a, 0u);
  EXPECT_NE(a, PublisherStreamIdForIdentity("account:bob"));
  EXPECT_EQ(PublisherStreamIdForIdentity(""), 1u);
}

TEST(CallHopAttachLogicTest, WaitIdleWhenNoWait) {
  SfuAttachWaitPollInput in;
  EXPECT_EQ(PollSfuAttachWait(in), SfuAttachWaitPollResult::Idle);
}

TEST(CallHopAttachLogicTest, WaitClearWhenAttached) {
  SfuAttachWaitPollInput in;
  in.wait_active = true;
  in.sfu_attached_for_call = true;
  EXPECT_EQ(PollSfuAttachWait(in), SfuAttachWaitPollResult::ClearAttached);
}

TEST(CallHopAttachLogicTest, WaitClearAsP2pWhenJoinedUnderThree) {
  SfuAttachWaitPollInput in;
  in.wait_active = true;
  in.joined_count = 2;
  in.media_active_mesh_for_call = true;
  EXPECT_EQ(PollSfuAttachWait(in), SfuAttachWaitPollResult::ClearAsP2p);
}

TEST(CallHopAttachLogicTest, WaitNoTimeoutWhileMigrateInFlight) {
  SfuAttachWaitPollInput in;
  in.wait_active = true;
  in.soft_migrate_in_flight = true;
  in.now_ms = 100;
  in.deadline_ms = 50;
  EXPECT_EQ(PollSfuAttachWait(in), SfuAttachWaitPollResult::Waiting);
}

TEST(CallHopAttachLogicTest, WaitTimeoutLeaveWhenPastDeadline) {
  SfuAttachWaitPollInput in;
  in.wait_active = true;
  in.now_ms = 100;
  in.deadline_ms = 50;
  EXPECT_EQ(PollSfuAttachWait(in), SfuAttachWaitPollResult::TimeoutLeave);
}

TEST(CallHopAttachLogicTest, WaitWaitingBeforeDeadline) {
  SfuAttachWaitPollInput in;
  in.wait_active = true;
  in.now_ms = 40;
  in.deadline_ms = 50;
  EXPECT_EQ(PollSfuAttachWait(in), SfuAttachWaitPollResult::Waiting);
}

TEST(CallHopAttachLogicTest, HopHintCapSkipsFailedAndDedupes) {
  auto capped = CapGuestHopPreferences({"a", "b", "a", "failed", "c"}, "failed", 3);
  ASSERT_EQ(capped.size(), 3u);
  EXPECT_EQ(capped[0], "a");
  EXPECT_EQ(capped[1], "b");
  EXPECT_EQ(capped[2], "c");
}

TEST(CallHopAttachLogicTest, HopHintOwnerRepickOnIntersection) {
  auto d = DecideHopHintOwnerAction({"x", "y"}, {"a", "y", "z"}, "failed");
  EXPECT_EQ(d.action, HopHintOwnerAction::RePick);
  EXPECT_EQ(d.preferred_hop_peer_id, "y");
}

TEST(CallHopAttachLogicTest, HopHintOwnerRefuseWhenNoIntersection) {
  auto d = DecideHopHintOwnerAction({"x", "y"}, {"a", "b"}, "failed");
  EXPECT_EQ(d.action, HopHintOwnerAction::RefuseGuest);
}

TEST(CallHopAttachLogicTest, HopHintOwnerSkipsFailedHopEvenIfPreferred) {
  auto d = DecideHopHintOwnerAction({"failed", "y"}, {"failed", "y"}, "failed");
  EXPECT_EQ(d.action, HopHintOwnerAction::RePick);
  EXPECT_EQ(d.preferred_hop_peer_id, "y");
}
