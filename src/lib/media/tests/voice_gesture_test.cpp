#include "lib/media/VoiceGesture.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using A = VoiceGesture::Action;
using P = VoiceGesture::Phase;

TEST(VoiceGesture, HoldAndReleaseSends) {
  VoiceGesture g(VoiceGesture::Mode::Hold);
  EXPECT_EQ(g.Press(500), A::Start);
  EXPECT_TRUE(g.Recording());
  g.Move(490);
  EXPECT_FALSE(g.InCancelZone());
  EXPECT_EQ(g.Release(), A::Send);
  EXPECT_EQ(g.phase(), P::Idle);
}

TEST(VoiceGesture, SlideUpThenReleaseCancels) {
  VoiceGesture g(VoiceGesture::Mode::Hold, 56.f);
  g.Press(500);
  g.Move(440);
  EXPECT_TRUE(g.InCancelZone());
  EXPECT_EQ(g.Release(), A::Cancel);
}

TEST(VoiceGesture, SlidingBackDownDisarmsTheCancel) {
  VoiceGesture g(VoiceGesture::Mode::Hold, 56.f);
  g.Press(500);
  g.Move(400);
  EXPECT_TRUE(g.InCancelZone());
  g.Move(480);
  EXPECT_FALSE(g.InCancelZone());
  EXPECT_EQ(g.Release(), A::Send);
}

TEST(VoiceGesture, LostGestureCancelsAndNeverSends) {
  VoiceGesture g(VoiceGesture::Mode::Hold);
  g.Press(500);
  EXPECT_EQ(g.Lost(), A::Cancel);
  EXPECT_EQ(g.phase(), P::Idle);
  EXPECT_EQ(g.Release(), A::None); // a release that arrives late sends nothing
}

TEST(VoiceGesture, LostWhenIdleDoesNothing) {
  VoiceGesture g;
  EXPECT_EQ(g.Lost(), A::None);
}

TEST(VoiceGesture, LimitReachedSwallowsTheRestOfThePress) {
  VoiceGesture g(VoiceGesture::Mode::Hold);
  g.Press(500);
  g.RecorderEnded(/*pointer_down=*/true); // the recording hit its limit while the finger is still down
  EXPECT_EQ(g.phase(), P::Swallowing);
  g.Move(100);                            // sliding while swallowed arms nothing
  EXPECT_FALSE(g.InCancelZone());
  EXPECT_EQ(g.Press(500), A::None);       // no second recording from the same press
  EXPECT_EQ(g.Release(), A::None);        // the lift does not send again
  EXPECT_EQ(g.phase(), P::Idle);
  EXPECT_EQ(g.Press(500), A::Start);      // the next press records again
}

TEST(VoiceGesture, LostWhileSwallowingJustEndsIt) {
  VoiceGesture g(VoiceGesture::Mode::Hold);
  g.Press(500);
  g.RecorderEnded(true);
  EXPECT_EQ(g.Lost(), A::None);
  EXPECT_EQ(g.phase(), P::Idle);
}

TEST(VoiceGesture, AnIncomingCallAfterTheFingerLiftedLeavesNothingToSwallow) {
  VoiceGesture g(VoiceGesture::Mode::Hold);
  g.Press(500);
  g.RecorderEnded(/*pointer_down=*/false);
  EXPECT_EQ(g.phase(), P::Idle);
}

TEST(VoiceGesture, ClickModeStartsThenSendOrCancelButtons) {
  VoiceGesture g(VoiceGesture::Mode::Click);
  EXPECT_EQ(g.Press(0), A::Start);
  EXPECT_EQ(g.Release(), A::None); // the mouse-up of the same click does nothing
  EXPECT_TRUE(g.Recording());
  EXPECT_EQ(g.SendNow(), A::Send);
  EXPECT_EQ(g.phase(), P::Idle);

  EXPECT_EQ(g.Press(0), A::Start);
  EXPECT_EQ(g.CancelNow(), A::Cancel);   // the Cancel button and Esc
  EXPECT_EQ(g.SendNow(), A::None);       // nothing left to send
}

TEST(VoiceGesture, ClickModeIgnoresSlidingAndEndsIdleWhenTheRecorderEnds) {
  VoiceGesture g(VoiceGesture::Mode::Click);
  g.Press(500);
  g.Move(0);
  EXPECT_FALSE(g.InCancelZone());
  g.RecorderEnded(true);
  EXPECT_EQ(g.phase(), P::Idle);
}

} // namespace
} // namespace pbr
