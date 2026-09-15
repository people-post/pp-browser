#include "feature/calls/CallDirectPlannerLogic.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(CallDirectPlannerLogicTest, ScheduleWhileHopDisallowedIgnoredWhenLive) {
  CallDirectPlannerApplyContext ctx;
  ctx.allows_direct_path = false;
  ctx.peer_nonempty = true;
  auto out = DecideCallDirectPlannerPhase(CallDirectPlannerPhase::Live,
                                          CallDirectPlannerEvent::ScheduleAnswerer, ctx);
  EXPECT_EQ(out.decision, CallDirectPlannerDecision::Ignore);
}

TEST(CallDirectPlannerLogicTest, ScheduleWhileHopDisallowedIgnoredFromIdle) {
  CallDirectPlannerApplyContext ctx;
  ctx.allows_direct_path = false;
  ctx.peer_nonempty = true;
  auto out = DecideCallDirectPlannerPhase(CallDirectPlannerPhase::Idle,
                                          CallDirectPlannerEvent::ScheduleAnswerer, ctx);
  EXPECT_EQ(out.decision, CallDirectPlannerDecision::Ignore);
}

TEST(CallDirectPlannerLogicTest, ScheduleAnswererArmsFromIdle) {
  CallDirectPlannerApplyContext ctx;
  ctx.allows_direct_path = true;
  ctx.peer_nonempty = true;
  auto out = DecideCallDirectPlannerPhase(CallDirectPlannerPhase::Idle,
                                          CallDirectPlannerEvent::ScheduleAnswerer, ctx);
  EXPECT_EQ(out.decision, CallDirectPlannerDecision::Transition);
  EXPECT_EQ(out.next, CallDirectPlannerPhase::Arming);
}

TEST(CallDirectPlannerLogicTest, ScheduleWithoutPeerIgnored) {
  CallDirectPlannerApplyContext ctx;
  ctx.allows_direct_path = true;
  auto out = DecideCallDirectPlannerPhase(CallDirectPlannerPhase::Idle,
                                          CallDirectPlannerEvent::ScheduleOfferer, ctx);
  EXPECT_EQ(out.decision, CallDirectPlannerDecision::Ignore);
}

TEST(CallDirectPlannerLogicTest, KeyReadyFromKeyWaitToConnecting) {
  CallDirectPlannerApplyContext ctx;
  ctx.allows_direct_path = true;
  ctx.has_media_key = true;
  auto out = DecideCallDirectPlannerPhase(CallDirectPlannerPhase::KeyWait,
                                          CallDirectPlannerEvent::KeyReady, ctx);
  EXPECT_EQ(out.decision, CallDirectPlannerDecision::Transition);
  EXPECT_EQ(out.next, CallDirectPlannerPhase::Connecting);
}

TEST(CallDirectPlannerLogicTest, KeyTimeoutClearsKeyWait) {
  CallDirectPlannerApplyContext ctx;
  auto out = DecideCallDirectPlannerPhase(CallDirectPlannerPhase::KeyWait,
                                          CallDirectPlannerEvent::KeyTimeout, ctx);
  EXPECT_EQ(out.decision, CallDirectPlannerDecision::Transition);
  EXPECT_EQ(out.next, CallDirectPlannerPhase::Idle);
}

TEST(CallDirectPlannerLogicTest, LateConnectSucceededIgnoredFromIdle) {
  CallDirectPlannerApplyContext ctx;
  ctx.allows_direct_path = true;
  auto out = DecideCallDirectPlannerPhase(CallDirectPlannerPhase::Idle,
                                          CallDirectPlannerEvent::ConnectSucceeded, ctx);
  EXPECT_EQ(out.decision, CallDirectPlannerDecision::Ignore);
}

TEST(CallDirectPlannerLogicTest, ConnectSucceededFromConnectingToLive) {
  CallDirectPlannerApplyContext ctx;
  ctx.allows_direct_path = true;
  auto out = DecideCallDirectPlannerPhase(CallDirectPlannerPhase::Connecting,
                                          CallDirectPlannerEvent::ConnectSucceeded, ctx);
  EXPECT_EQ(out.decision, CallDirectPlannerDecision::Transition);
  EXPECT_EQ(out.next, CallDirectPlannerPhase::Live);
}

TEST(CallDirectPlannerLogicTest, TxOnlyOnlyFromLive) {
  CallDirectPlannerApplyContext ctx;
  auto bad = DecideCallDirectPlannerPhase(CallDirectPlannerPhase::Connecting,
                                          CallDirectPlannerEvent::TxOnlyGraceExpired, ctx);
  EXPECT_EQ(bad.decision, CallDirectPlannerDecision::Ignore);
  auto ok = DecideCallDirectPlannerPhase(CallDirectPlannerPhase::Live,
                                         CallDirectPlannerEvent::TxOnlyGraceExpired, ctx);
  EXPECT_EQ(ok.decision, CallDirectPlannerDecision::Transition);
  EXPECT_EQ(ok.next, CallDirectPlannerPhase::DegradedTxOnly);
}

TEST(CallDirectPlannerLogicTest, StopFromConnectingToIdle) {
  CallDirectPlannerApplyContext ctx;
  auto out = DecideCallDirectPlannerPhase(CallDirectPlannerPhase::Connecting,
                                          CallDirectPlannerEvent::Stop, ctx);
  EXPECT_EQ(out.decision, CallDirectPlannerDecision::Transition);
  EXPECT_EQ(out.next, CallDirectPlannerPhase::Idle);
}

TEST(CallDirectPlannerLogicTest, StoppingIgnoresConnectSucceeded) {
  CallDirectPlannerApplyContext ctx;
  ctx.stopping = true;
  ctx.allows_direct_path = true;
  auto out = DecideCallDirectPlannerPhase(CallDirectPlannerPhase::Connecting,
                                          CallDirectPlannerEvent::ConnectSucceeded, ctx);
  EXPECT_EQ(out.decision, CallDirectPlannerDecision::Ignore);
}

TEST(CallDirectPlannerLogicTest, PhaseAfterArm) {
  EXPECT_EQ(DirectPlannerPhaseAfterArm(true, false), CallDirectPlannerPhase::Connecting);
  EXPECT_EQ(DirectPlannerPhaseAfterArm(false, false), CallDirectPlannerPhase::KeyWait);
  EXPECT_EQ(DirectPlannerPhaseAfterArm(false, true), CallDirectPlannerPhase::Connecting);
}

} // namespace
} // namespace pbr
