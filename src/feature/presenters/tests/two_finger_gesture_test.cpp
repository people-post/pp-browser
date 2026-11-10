// Fingers on the avatar crop page (PLAN-avatar-edit item 8): one finger drags, two pinch and turn about their
// midpoint, nothing jumps when a finger lifts, is reused or is cancelled.
#include "feature/presenters/TwoFingerGesture.h"

#include <gtest/gtest.h>

#include <cmath>

namespace pbr {
namespace {

constexpr double kPi = 3.14159265358979323846;
using K = FingerEventKind;

double Deg(const double d) {
  return d * kPi / 180.0;
}

// The sum of several deltas: pans add, scales multiply, turns add.
struct Total {
  double dx = 0.0, dy = 0.0, scale = 1.0, rotation = 0.0;
  void Add(const FingerGestureDelta& d) {
    dx += d.pan_dx;
    dy += d.pan_dy;
    scale *= d.scale;
    rotation += d.rotation;
  }
};

TEST(TwoFingerGestureTest, OneFingerPans) {
  TwoFingerGesture g;
  FingerGestureDelta d = g.OnEvent(7, 10, 20, K::Down);
  EXPECT_EQ(d.pan_dx, 0.0);
  EXPECT_FALSE(d.HasPinch());
  d = g.OnEvent(7, 13, 16, K::Move);
  EXPECT_DOUBLE_EQ(d.pan_dx, 3.0);
  EXPECT_DOUBLE_EQ(d.pan_dy, -4.0);
  EXPECT_FALSE(d.HasPinch());
  EXPECT_DOUBLE_EQ(d.focus_x, 13.0);
  EXPECT_DOUBLE_EQ(d.focus_y, 16.0);
  g.OnEvent(7, 13, 16, K::Up);
  EXPECT_EQ(g.TrackedCount(), 0);
}

TEST(TwoFingerGestureTest, TwoFingersMovingApartArePurePinch) {
  TwoFingerGesture g;
  g.OnEvent(1, -50, 0, K::Down);
  g.OnEvent(2, 50, 0, K::Down);
  Total t;
  t.Add(g.OnEvent(1, -75, 0, K::Move));
  t.Add(g.OnEvent(2, 75, 0, K::Move));
  EXPECT_NEAR(t.scale, 1.5, 1e-12);
  EXPECT_NEAR(t.rotation, 0.0, 1e-12);
  EXPECT_NEAR(t.dx, 0.0, 1e-12) << "the midpoint ends where it started";
  EXPECT_NEAR(t.dy, 0.0, 1e-12);
}

TEST(TwoFingerGestureTest, TwoFingersCirclingArePureTurn) {
  TwoFingerGesture g;
  g.OnEvent(1, -50, 0, K::Down);
  g.OnEvent(2, 50, 0, K::Down);
  Total t;
  const double a = Deg(40);
  t.Add(g.OnEvent(2, 50 * std::cos(a), 50 * std::sin(a), K::Move));
  t.Add(g.OnEvent(1, -50 * std::cos(a), -50 * std::sin(a), K::Move));
  EXPECT_NEAR(t.rotation, a, 1e-12) << "y down: a growing angle is clockwise, positive";
  EXPECT_NEAR(t.scale, 1.0, 1e-12);
  EXPECT_NEAR(t.dx, 0.0, 1e-12);
  EXPECT_NEAR(t.dy, 0.0, 1e-12);
}

TEST(TwoFingerGestureTest, PinchTurnAndMoveTogether) {
  TwoFingerGesture g;
  g.OnEvent(1, 0, 0, K::Down);
  g.OnEvent(2, 100, 0, K::Down);
  // End: midpoint (50,0) -> (70, 30), distance 100 -> 160, angle 0 -> -25 degrees (counter-clockwise).
  const double a = Deg(-25);
  const double hx = 80 * std::cos(a);
  const double hy = 80 * std::sin(a);
  Total t;
  const FingerGestureDelta d1 = g.OnEvent(2, 70 + hx, 30 + hy, K::Move);
  t.Add(d1);
  const FingerGestureDelta d2 = g.OnEvent(1, 70 - hx, 30 - hy, K::Move);
  t.Add(d2);
  EXPECT_NEAR(t.dx, 20.0, 1e-9);
  EXPECT_NEAR(t.dy, 30.0, 1e-9);
  EXPECT_NEAR(t.scale, 1.6, 1e-9);
  EXPECT_NEAR(t.rotation, a, 1e-9);
  EXPECT_NEAR(d2.focus_x, 70.0, 1e-9) << "the focus is the new midpoint";
  EXPECT_NEAR(d2.focus_y, 30.0, 1e-9);
}

TEST(TwoFingerGestureTest, ATurnAcrossTheBackOfTheCircleWrapsToTheShortWay) {
  TwoFingerGesture g;
  g.OnEvent(1, 0, 0, K::Down);
  g.OnEvent(2, -100, 1, K::Down); // the line points at about +179 degrees
  // About -179: a 1.15 degree turn, not 358. Pointing left and moving up is clockwise on a y-down screen: positive.
  const FingerGestureDelta d = g.OnEvent(2, -100, -1, K::Move);
  EXPECT_LT(std::abs(d.rotation), Deg(2));
  EXPECT_GT(d.rotation, 0.0);
}

TEST(TwoFingerGestureTest, LiftingOneOfTwoFingersCarriesOnAsAPanWithoutAJump) {
  TwoFingerGesture g;
  g.OnEvent(1, 0, 0, K::Down);
  g.OnEvent(2, 100, 0, K::Down);
  g.OnEvent(2, 120, 10, K::Move);
  const FingerGestureDelta up = g.OnEvent(1, 0, 0, K::Up);
  EXPECT_EQ(up.pan_dx, 0.0);
  EXPECT_EQ(up.pan_dy, 0.0);
  EXPECT_FALSE(up.HasPinch());
  EXPECT_EQ(g.TrackedCount(), 1);
  const FingerGestureDelta d = g.OnEvent(2, 125, 12, K::Move);
  EXPECT_DOUBLE_EQ(d.pan_dx, 5.0) << "from where the finger was, not from the old midpoint";
  EXPECT_DOUBLE_EQ(d.pan_dy, 2.0);
  EXPECT_FALSE(d.HasPinch());
}

TEST(TwoFingerGestureTest, AThirdFingerIsIgnored) {
  TwoFingerGesture g;
  g.OnEvent(1, 0, 0, K::Down);
  g.OnEvent(2, 100, 0, K::Down);
  const FingerGestureDelta down3 = g.OnEvent(3, 50, 50, K::Down);
  EXPECT_EQ(g.TrackedCount(), 2);
  EXPECT_EQ(down3.pan_dx, 0.0);
  const FingerGestureDelta move3 = g.OnEvent(3, 300, 300, K::Move);
  EXPECT_EQ(move3.pan_dx, 0.0);
  EXPECT_FALSE(move3.HasPinch());
  g.OnEvent(1, 0, 0, K::Up);
  EXPECT_EQ(g.OnEvent(3, 310, 310, K::Move).pan_dx, 0.0) << "not promoted when a tracked finger lifts";
}

TEST(TwoFingerGestureTest, AReusedFingerIdStartsFresh) {
  TwoFingerGesture g;
  g.OnEvent(4, 0, 0, K::Down);
  g.OnEvent(4, 10, 0, K::Move);
  g.OnEvent(4, 10, 0, K::Up);
  g.OnEvent(4, 300, 200, K::Down); // the OS hands the same id to a new touch far away
  const FingerGestureDelta d = g.OnEvent(4, 302, 199, K::Move);
  EXPECT_DOUBLE_EQ(d.pan_dx, 2.0);
  EXPECT_DOUBLE_EQ(d.pan_dy, -1.0);
  // A repeated Down for a tracked id moves it without a delta.
  const FingerGestureDelta again = g.OnEvent(4, 0, 0, K::Down);
  EXPECT_EQ(again.pan_dx, 0.0);
  EXPECT_EQ(g.TrackedCount(), 1);
  EXPECT_DOUBLE_EQ(g.OnEvent(4, 1, 0, K::Move).pan_dx, 1.0);
}

TEST(TwoFingerGestureTest, AfterACancelASingleFingerDragDoesNotJump) {
  TwoFingerGesture g;
  g.OnEvent(1, 0, 0, K::Down);
  g.OnEvent(2, 100, 0, K::Down);
  g.OnEvent(5, 0, 0, K::Cancel); // a call came in: SDL_EVENT_FINGER_CANCELED
  EXPECT_EQ(g.TrackedCount(), 0);
  g.OnEvent(1, 400, 400, K::Down);
  const FingerGestureDelta d = g.OnEvent(1, 404, 397, K::Move);
  EXPECT_DOUBLE_EQ(d.pan_dx, 4.0);
  EXPECT_DOUBLE_EQ(d.pan_dy, -3.0);
  EXPECT_FALSE(d.HasPinch()) << "no stale second finger";
  g.Reset();
  EXPECT_EQ(g.TrackedCount(), 0);
  EXPECT_EQ(g.OnEvent(1, 500, 500, K::Move).pan_dx, 0.0) << "after Reset the finger is unknown";
}

TEST(TwoFingerGestureTest, FingersTooCloseTogetherHoldScaleAndTurn) {
  TwoFingerGesture g;
  g.OnEvent(1, 0, 0, K::Down);
  g.OnEvent(2, 3, 0, K::Down); // 3 dp apart
  FingerGestureDelta d = g.OnEvent(2, 0, 3, K::Move);
  EXPECT_DOUBLE_EQ(d.scale, 1.0);
  EXPECT_DOUBLE_EQ(d.rotation, 0.0);
  d = g.OnEvent(2, 60, 0, K::Move); // 3 -> 60 dp: would be a 20x zoom
  EXPECT_DOUBLE_EQ(d.scale, 1.0);
  EXPECT_DOUBLE_EQ(d.rotation, 0.0);
  EXPECT_NEAR(d.pan_dx, 30.0, 1e-12) << "the midpoint still pans";
  d = g.OnEvent(2, 90, 0, K::Move); // separated: pinch resumes from here
  EXPECT_NEAR(d.scale, 1.5, 1e-12);
}

} // namespace
} // namespace pbr
