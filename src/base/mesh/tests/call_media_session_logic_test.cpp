#include "base/mesh/l4/call_media/CallMediaSessionLogic.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(CallMediaSessionLogicTest, ConnectTimeoutReturnsIdle) {
  // SESSION_MACHINES golden #7 (pure): ConnectTimeout → Idle from Dialing/HelloOutbound.
  auto dialing =
      DecideCallMediaSessionPhase(CallMediaSessionPhase::Dialing, CallMediaSessionEvent::ConnectTimeout);
  EXPECT_EQ(dialing.decision, CallMediaSessionPhaseDecision::Transition);
  EXPECT_EQ(dialing.next, CallMediaSessionPhase::Idle);

  auto hello = DecideCallMediaSessionPhase(CallMediaSessionPhase::HelloOutbound,
                                           CallMediaSessionEvent::ConnectTimeout);
  EXPECT_EQ(hello.decision, CallMediaSessionPhaseDecision::Transition);
  EXPECT_EQ(hello.next, CallMediaSessionPhase::Idle);
}

TEST(CallMediaSessionLogicTest, LateOpenStreamOkIgnoredWhenIdle) {
  // SESSION_MACHINES golden #7: after timeout → Idle; late OpenStreamOk ignored.
  auto late =
      DecideCallMediaSessionPhase(CallMediaSessionPhase::Idle, CallMediaSessionEvent::OpenStreamOk);
  EXPECT_EQ(late.decision, CallMediaSessionPhaseDecision::Ignore);

  auto detaching =
      DecideCallMediaSessionPhase(CallMediaSessionPhase::Detaching, CallMediaSessionEvent::OpenStreamOk);
  EXPECT_EQ(detaching.decision, CallMediaSessionPhaseDecision::Ignore);
}

TEST(CallMediaSessionLogicTest, OpenStreamOkResumesDialingWhenWaiterActive) {
  CallMediaSessionApplyContext ctx;
  ctx.connect_waiter_active = true;
  auto out = DecideCallMediaSessionPhase(CallMediaSessionPhase::Idle,
                                         CallMediaSessionEvent::OpenStreamOk, ctx);
  EXPECT_EQ(out.decision, CallMediaSessionPhaseDecision::Transition);
  EXPECT_EQ(out.next, CallMediaSessionPhase::Dialing);
}

TEST(CallMediaSessionLogicTest, FailNotifySuppressedWhenIdleOrDetaching) {
  // SESSION_MACHINES golden #5 / SoftMigrate ReleaseDirect.
  EXPECT_TRUE(CallMediaFailNotifySuppressed(CallMediaSessionPhase::Idle));
  EXPECT_TRUE(CallMediaFailNotifySuppressed(CallMediaSessionPhase::Detaching));
  EXPECT_FALSE(CallMediaFailNotifySuppressed(CallMediaSessionPhase::MediaReady));
  EXPECT_FALSE(CallMediaFailNotifySuppressed(CallMediaSessionPhase::Dialing));
}

TEST(CallMediaSessionLogicTest, DetachFromDialingGoesIdle) {
  auto out = DecideCallMediaSessionPhase(CallMediaSessionPhase::Dialing,
                                         CallMediaSessionEvent::DetachRequested);
  EXPECT_EQ(out.decision, CallMediaSessionPhaseDecision::Transition);
  EXPECT_EQ(out.next, CallMediaSessionPhase::Idle);
}

TEST(CallMediaSessionLogicTest, LocalWinsCallMediaGlareIsTotalOrder) {
  EXPECT_TRUE(LocalWinsCallMediaGlare("b", "a"));
  EXPECT_FALSE(LocalWinsCallMediaGlare("a", "b"));
  EXPECT_FALSE(LocalWinsCallMediaGlare("same", "same"));
  EXPECT_FALSE(LocalWinsCallMediaGlare("peer", ""));
  EXPECT_FALSE(LocalWinsCallMediaGlare("", "peer"));
  EXPECT_FALSE(LocalWinsCallMediaGlare("", ""));
}

} // namespace
} // namespace pbr
