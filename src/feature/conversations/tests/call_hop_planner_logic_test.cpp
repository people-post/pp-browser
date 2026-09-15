#include "feature/calls/CallHopPlannerLogic.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(CallHopPlannerLogicTest, LocalAcceptN3WithoutNIgnored) {
  CallHopPlannerApplyContext ctx;
  auto out = DecideCallHopPlannerPhase(CallHopPlannerPhase::Idle, CallHopPlannerEvent::LocalAcceptN3, ctx);
  EXPECT_EQ(out.decision, CallHopPlannerDecision::Ignore);
}

TEST(CallHopPlannerLogicTest, LocalAcceptN3ToWaiting) {
  CallHopPlannerApplyContext ctx;
  ctx.should_arm_hop = true;
  auto out = DecideCallHopPlannerPhase(CallHopPlannerPhase::Idle, CallHopPlannerEvent::LocalAcceptN3, ctx);
  EXPECT_EQ(out.decision, CallHopPlannerDecision::Transition);
  EXPECT_EQ(out.next, CallHopPlannerPhase::WaitingAttach);
}

TEST(CallHopPlannerLogicTest, LocalAcceptWithHintToAttaching) {
  CallHopPlannerApplyContext ctx;
  ctx.should_arm_hop = true;
  ctx.has_sfu_hint = true;
  auto out = DecideCallHopPlannerPhase(CallHopPlannerPhase::Idle, CallHopPlannerEvent::LocalAcceptN3, ctx);
  EXPECT_EQ(out.next, CallHopPlannerPhase::Attaching);
}

TEST(CallHopPlannerLogicTest, InboundSfuIgnoredWhenStatusDisallowsHop) {
  CallHopPlannerApplyContext ctx;
  ctx.allows_hop_path = false;
  auto out =
      DecideCallHopPlannerPhase(CallHopPlannerPhase::Idle, CallHopPlannerEvent::SfuAttachInbound, ctx);
  EXPECT_EQ(out.decision, CallHopPlannerDecision::Ignore);
}

TEST(CallHopPlannerLogicTest, SoftMigrateToMigrating) {
  CallHopPlannerApplyContext ctx;
  ctx.should_arm_hop = true;
  auto out = DecideCallHopPlannerPhase(CallHopPlannerPhase::WaitingAttach,
                                       CallHopPlannerEvent::SoftMigrateRequested, ctx);
  EXPECT_EQ(out.decision, CallHopPlannerDecision::Transition);
  EXPECT_EQ(out.next, CallHopPlannerPhase::Migrating);
}

TEST(CallHopPlannerLogicTest, AttachSucceededToLive) {
  CallHopPlannerApplyContext ctx;
  auto out =
      DecideCallHopPlannerPhase(CallHopPlannerPhase::Attaching, CallHopPlannerEvent::AttachSucceeded, ctx);
  EXPECT_EQ(out.next, CallHopPlannerPhase::Live);
}

TEST(CallHopPlannerLogicTest, AttachWaitExpiredClears) {
  CallHopPlannerApplyContext ctx;
  auto out = DecideCallHopPlannerPhase(CallHopPlannerPhase::WaitingAttach,
                                       CallHopPlannerEvent::AttachWaitExpired, ctx);
  EXPECT_EQ(out.next, CallHopPlannerPhase::Idle);
}

TEST(CallHopPlannerLogicTest, LateAttachSucceededFromIdleIgnored) {
  CallHopPlannerApplyContext ctx;
  auto out =
      DecideCallHopPlannerPhase(CallHopPlannerPhase::Idle, CallHopPlannerEvent::AttachSucceeded, ctx);
  EXPECT_EQ(out.decision, CallHopPlannerDecision::Ignore);
}

} // namespace
} // namespace pbr
