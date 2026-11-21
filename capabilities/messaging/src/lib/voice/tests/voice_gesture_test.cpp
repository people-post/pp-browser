#include "lib/voice/VoiceGesture.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using A = VoiceGesture::Action;
using P = VoiceGesture::Phase;

TEST(VoiceGesture, HoldAndReleaseSends) {
  VoiceGesture g(VoiceGesture::Mode::Hold);
  EXPECT_EQ(g.Press(100, 500), A::Start);
  EXPECT_TRUE(g.Recording());
  g.Move(100, 490);
  EXPECT_FALSE(g.InCancelZone());
  EXPECT_EQ(g.Release(), A::Send);
  EXPECT_EQ(g.phase(), P::Idle);
}

TEST(VoiceGesture, SlideUpThenReleaseCancels) {
  VoiceGesture g(VoiceGesture::Mode::Hold, 56.f);
  g.Press(100, 500);
  g.Move(100, 440);
  EXPECT_TRUE(g.InCancelZone());
  EXPECT_EQ(g.Release(), A::Cancel);
}

TEST(VoiceGesture, SlidingBackDownDisarmsTheCancel) {
  VoiceGesture g(VoiceGesture::Mode::Hold, 56.f);
  g.Press(100, 500);
  g.Move(100, 400);
  EXPECT_TRUE(g.InCancelZone());
  g.Move(100, 480);
  EXPECT_FALSE(g.InCancelZone());
  EXPECT_EQ(g.Release(), A::Send);
}

TEST(VoiceGesture, LostGestureCancelsAndNeverSends) {
  VoiceGesture g(VoiceGesture::Mode::Hold);
  g.Press(100, 500);
  EXPECT_EQ(g.Lost(), A::Cancel);
  EXPECT_EQ(g.phase(), P::Idle);
  EXPECT_EQ(g.Release(), A::None); // a release that arrives late sends nothing
}

TEST(VoiceGesture, LostWhenIdleDoesNothing) {
  VoiceGesture g;
  EXPECT_EQ(g.Lost(), A::None);
}

TEST(VoiceGesture, AFailureSwallowsTheRestOfThePress) {
  VoiceGesture g(VoiceGesture::Mode::Hold);
  g.Press(100, 500);
  g.RecorderEnded(/*pointer_down=*/true); // the recorder failed while the finger is still down
  EXPECT_EQ(g.phase(), P::Swallowing);
  g.Move(100, 100); // sliding while swallowed arms nothing
  EXPECT_FALSE(g.InCancelZone());
  EXPECT_EQ(g.Press(100, 500), A::None); // no second recording from the same press
  EXPECT_EQ(g.Release(), A::None);       // the lift does not send again
  EXPECT_EQ(g.phase(), P::Idle);
  EXPECT_EQ(g.Press(100, 500), A::Start); // the next press records again
}

// The limit under a held finger: the recording stops, the release decides.
TEST(VoiceGesture, AtTheLimitUnderAFingerTheReleaseSendsAndASlideUpCancels) {
  VoiceGesture g(VoiceGesture::Mode::Hold);
  g.Press(100, 500);
  g.RecorderLimit(/*pointer_down=*/true);
  EXPECT_EQ(g.phase(), P::LimitHold);
  EXPECT_TRUE(g.AtLimit());
  EXPECT_FALSE(g.Recording());
  g.Move(100, 495);
  EXPECT_EQ(g.Release(), A::Send);
  EXPECT_EQ(g.phase(), P::Idle);

  g.Press(100, 500);
  g.RecorderLimit(true);
  g.Move(100, 400); // slide up still arms
  EXPECT_TRUE(g.InCancelZone());
  EXPECT_EQ(g.Release(), A::Cancel);
}

TEST(VoiceGesture, AtTheLimitAfterTheFingerLiftedThereIsNothingToWaitFor) {
  VoiceGesture g(VoiceGesture::Mode::Hold);
  g.Press(100, 500);
  g.RecorderLimit(/*pointer_down=*/false);
  EXPECT_EQ(g.phase(), P::Idle);
}

TEST(VoiceGesture, LostAtTheLimitCancelsAndNeverSends) {
  VoiceGesture g(VoiceGesture::Mode::Hold);
  g.Press(100, 500);
  g.RecorderLimit(true);
  EXPECT_EQ(g.Lost(), A::Cancel);
  EXPECT_EQ(g.Release(), A::None);
}

// The cancel line is 56dp up from the press; 48dp keeps it armed; sideways never arms or disarms.
TEST(VoiceGesture, TheCancelArmsAt56UpAndLetsGoUnder48WithAHysteresis) {
  VoiceGesture g(VoiceGesture::Mode::Hold);
  g.Press(100, 500);
  g.Move(100, 445.5f); // 54.5 up
  EXPECT_FALSE(g.InCancelZone());
  g.Move(100, 444.f); // 56 up
  EXPECT_TRUE(g.InCancelZone());
  g.Move(100, 450.f); // 50 up: still armed (hysteresis)
  EXPECT_TRUE(g.InCancelZone());
  g.Move(100, 452.5f); // 47.5 up: let go
  EXPECT_FALSE(g.InCancelZone());
  g.Move(100, 450.f); // 50 up again: not armed again until 56
  EXPECT_FALSE(g.InCancelZone());
}

TEST(VoiceGesture, MovingSidewaysNeverChangesTheCancel) {
  VoiceGesture g(VoiceGesture::Mode::Hold);
  g.Press(100, 500);
  g.Move(380, 500);
  g.Move(5, 495);
  EXPECT_FALSE(g.InCancelZone());
  g.Move(5, 400);
  g.Move(380, 400);
  g.Move(5, 400);
  EXPECT_TRUE(g.InCancelZone());
}

// One follow formula: the anchor only moves once the finger is more than 24dp away from it.
TEST(VoiceGesture, TheAnchorFollowsOnlyPastTheDeadZone) {
  VoiceGesture g(VoiceGesture::Mode::Hold);
  g.Press(100, 500);
  EXPECT_FLOAT_EQ(g.anchor_x(), 100.f);
  g.Move(120, 500); // 20 away: inside
  EXPECT_FLOAT_EQ(g.anchor_x(), 100.f);
  g.Move(124, 500); // exactly the edge
  EXPECT_FLOAT_EQ(g.anchor_x(), 100.f);
  g.Move(150, 500); // outside: dragged along, keeping 24 behind the finger
  EXPECT_FLOAT_EQ(g.anchor_x(), 126.f);
  g.Move(110, 500);                     // finger comes back past the anchor's other side
  EXPECT_FLOAT_EQ(g.anchor_x(), 126.f); // still inside 24dp of the finger: stays
  g.Move(90, 500);
  EXPECT_FLOAT_EQ(g.anchor_x(), 114.f);
}

TEST(VoiceBubbleGeometry, TheBubbleStaysOnTheScreenAndTheTailPointsAtTheAnchor) {
  const float width = 393.f;
  // Left end, middle, right end of the screen.
  EXPECT_FLOAT_EQ(VoiceBubbleLeft(20.f, width), 12.f);     // clamped to the left margin
  EXPECT_FLOAT_EQ(VoiceBubbleLeft(196.5f, width), 108.5f); // centred on the anchor
  EXPECT_FLOAT_EQ(VoiceBubbleLeft(380.f, width), 205.f);   // clamped: 393 - 176 - 12
  // The tail: relative to the bubble, never closer than 20dp to a side (176 wide).
  EXPECT_FLOAT_EQ(VoiceBubbleTail(20.f, 12.f), 20.f);
  EXPECT_FLOAT_EQ(VoiceBubbleTail(196.5f, 108.5f), 88.f);
  EXPECT_FLOAT_EQ(VoiceBubbleTail(380.f, 205.f), 156.f);
  // A very narrow window does not make the clamp invert.
  EXPECT_FLOAT_EQ(VoiceBubbleLeft(50.f, 190.f), 12.f);
  // The bubble's bottom is 98dp above the press: the cancel line + finger radius + spare.
  EXPECT_FLOAT_EQ(voice_dp::kBubbleLift, 98.f);
}

TEST(VoiceGesture, LostWhileSwallowingJustEndsIt) {
  VoiceGesture g(VoiceGesture::Mode::Hold);
  g.Press(100, 500);
  g.RecorderEnded(true);
  EXPECT_EQ(g.Lost(), A::None);
  EXPECT_EQ(g.phase(), P::Idle);
}

TEST(VoiceGesture, AnIncomingCallAfterTheFingerLiftedLeavesNothingToSwallow) {
  VoiceGesture g(VoiceGesture::Mode::Hold);
  g.Press(100, 500);
  g.RecorderEnded(/*pointer_down=*/false);
  EXPECT_EQ(g.phase(), P::Idle);
}

TEST(VoiceGesture, ClickModeStartsThenSendOrCancelButtons) {
  VoiceGesture g(VoiceGesture::Mode::Click);
  EXPECT_EQ(g.Press(100, 0), A::Start);
  EXPECT_EQ(g.Release(), A::None); // the mouse-up of the same click does nothing
  EXPECT_TRUE(g.Recording());
  EXPECT_EQ(g.SendNow(), A::Send);
  EXPECT_EQ(g.phase(), P::Idle);

  EXPECT_EQ(g.Press(100, 0), A::Start);
  EXPECT_EQ(g.CancelNow(), A::Cancel); // the Cancel button and Esc
  EXPECT_EQ(g.SendNow(), A::None);     // nothing left to send
}

TEST(VoiceGesture, ClickModeIgnoresSlidingAndEndsIdleWhenTheRecorderEnds) {
  VoiceGesture g(VoiceGesture::Mode::Click);
  g.Press(100, 500);
  g.Move(100, 0);
  EXPECT_FALSE(g.InCancelZone());
  g.RecorderEnded(true);
  EXPECT_EQ(g.phase(), P::Idle);
}

} // namespace
} // namespace pbr
