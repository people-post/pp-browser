// Finger deltas summed per frame (PLAN-avatar-edit item 9): a pure two-finger twist, which SDL delivers as two separate
// finger events, must leave the scale alone even where the clamp at scale 1 would eat the first half of it.
#include "feature/presenters/CropGestureAccumulator.h"

#include <gtest/gtest.h>

#include <cmath>

namespace pbr {
namespace {

constexpr double kPi = 3.14159265358979323846;
using K = FingerEventKind;

// Two fingers 100 dp apart on a horizontal line, then turned by `degrees` about their midpoint: finger 1 moves first,
// finger 2 second (one finger per event, as SDL reports it).
void Twist(TwoFingerGesture& g, CropGestureAccumulator& acc, const double degrees, const bool accumulate_per_frame,
           AvatarCropFrame& frame) {
  const double t = degrees * kPi / 180.0;
  const double x = 50.0 * std::cos(t);
  const double y = 50.0 * std::sin(t);
  const FingerGestureDelta first = g.OnEvent(1, -x, -y, K::Move);
  if (!accumulate_per_frame) {
    frame.Pan(first.pan_dx, first.pan_dy);
    if (first.HasPinch()) {
      frame.Pinch({first.focus_x, first.focus_y}, first.scale, first.rotation);
    }
  } else {
    acc.Add(first);
  }
  const FingerGestureDelta second = g.OnEvent(2, x, y, K::Move);
  if (!accumulate_per_frame) {
    frame.Pan(second.pan_dx, second.pan_dy);
    if (second.HasPinch()) {
      frame.Pinch({second.focus_x, second.focus_y}, second.scale, second.rotation);
    }
  } else {
    acc.Add(second);
  }
}

TEST(CropGestureAccumulatorTest, ApplyingNothingTouchesNothing) {
  AvatarCropFrame frame(400, 300, 200);
  CropGestureAccumulator acc;
  EXPECT_FALSE(acc.Pending());
  EXPECT_FALSE(acc.ApplyTo(frame));
  EXPECT_DOUBLE_EQ(frame.scale(), 1.0);
}

TEST(CropGestureAccumulatorTest, ATwistAtScaleOneKeepsTheScaleWhenSummedPerFrame) {
  AvatarCropFrame frame(400, 300, 200);
  TwoFingerGesture g;
  CropGestureAccumulator acc;
  g.OnEvent(1, -50, 0, K::Down);
  g.OnEvent(2, 50, 0, K::Down);
  Twist(g, acc, 30.0, true, frame);
  ASSERT_TRUE(acc.Pending());
  EXPECT_TRUE(acc.ApplyTo(frame));
  EXPECT_FALSE(acc.Pending());
  EXPECT_DOUBLE_EQ(frame.scale(), 1.0);
  EXPECT_NEAR(frame.theta(), 30.0 * kPi / 180.0, 1e-9);
}

// Documents why the sum is needed: at scale 1 the first step (distance 100 -> 91) is clamped away and the second
// (91 -> 100) grows the picture.
TEST(CropGestureAccumulatorTest, ApplyingEachStepAloneDriftsAtScaleOne) {
  AvatarCropFrame frame(400, 300, 200);
  TwoFingerGesture g;
  CropGestureAccumulator acc;
  g.OnEvent(1, -50, 0, K::Down);
  g.OnEvent(2, 50, 0, K::Down);
  Twist(g, acc, 30.0, false, frame);
  EXPECT_GT(frame.scale(), 1.0001);
}

TEST(CropGestureAccumulatorTest, PansAddAndAFrameOfOneFingerPansOnce) {
  AvatarCropFrame frame(1000, 500, 200);
  frame.Pinch({0, 0}, 2.0, 0.0); // room to move
  const AvatarCropFrame::Point before = frame.centre();
  TwoFingerGesture g;
  CropGestureAccumulator acc;
  g.OnEvent(5, 10, 10, K::Down);
  acc.Add(g.OnEvent(5, 12, 10, K::Move));
  acc.Add(g.OnEvent(5, 15, 10, K::Move));
  EXPECT_TRUE(acc.ApplyTo(frame));
  AvatarCropFrame reference(1000, 500, 200);
  reference.Pinch({0, 0}, 2.0, 0.0);
  reference.Pan(5.0, 0.0);
  EXPECT_NEAR(frame.centre().x, reference.centre().x, 1e-9);
  EXPECT_NE(frame.centre().x, before.x);
  EXPECT_DOUBLE_EQ(frame.scale(), 2.0);
}

TEST(CropGestureAccumulatorTest, ResetForgetsWhatWasAdded) {
  AvatarCropFrame frame(400, 300, 200);
  CropGestureAccumulator acc;
  FingerGestureDelta d;
  d.scale = 2.0;
  acc.Add(d);
  acc.Reset();
  EXPECT_FALSE(acc.ApplyTo(frame));
  EXPECT_DOUBLE_EQ(frame.scale(), 1.0);
}

} // namespace
} // namespace pbr
