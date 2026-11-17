#include "foundation/platform/ScreenKeyboardGeometry.h"

#include <gtest/gtest.h>

#include <cmath>

namespace pbr {
namespace {

TEST(ScreenKeyboardGeometryTest, IphoneFullScreenDockedKeyboard) {
  // 393x852 window, keyboard 336 tall at the bottom: the same number as the screen-based maths gave.
  EXPECT_EQ(KeyboardInsetInWindow({0, 516, 393, 336}, 393, 852), 336);
}

TEST(ScreenKeyboardGeometryTest, IpadWindowNotTouchingTheScreenBottom) {
  // Window 820 tall inside a 1180 screen, 100 pt above the bottom; the keyboard (screen y 880..1180) reaches
  // 240 pt into it, in window coordinates y 580..880 (clipped to 820).
  EXPECT_EQ(KeyboardInsetInWindow({0, 580, 820, 300}, 820, 820), 240);
}

TEST(ScreenKeyboardGeometryTest, KeyboardBelowTheWindowCoversNothing) {
  EXPECT_EQ(KeyboardInsetInWindow({0, 900, 820, 300}, 820, 820), 0);
}

TEST(ScreenKeyboardGeometryTest, FloatingKeyboardIsLeftAlone) {
  EXPECT_EQ(KeyboardInsetInWindow({200, 300, 320, 260}, 820, 1000), 0);
}

TEST(ScreenKeyboardGeometryTest, ShortcutBarOnlyLiftsByItsHeight) {
  EXPECT_EQ(KeyboardInsetInWindow({0, 962, 820, 38}, 820, 1000), 38);
}

TEST(ScreenKeyboardGeometryTest, KeyboardPartlyOutsideTheWindowIsClipped) {
  // Narrow window beside the keyboard's left edge: still covered at the bottom; and a keyboard taller than the window.
  EXPECT_EQ(KeyboardInsetInWindow({-200, 700, 800, 300}, 500, 1000), 300);
  EXPECT_EQ(KeyboardInsetInWindow({0, -50, 500, 400}, 500, 300), 300);
}

TEST(ScreenKeyboardGeometryTest, HiddenOrEmptyKeyboardIsZero) {
  EXPECT_EQ(KeyboardInsetInWindow({}, 820, 1000), 0);
  EXPECT_EQ(KeyboardInsetInWindow({0, 1000, 820, 0}, 820, 1000), 0);
}

// KeyboardLift(window_h, keyboard_top, input_bottom, gap), all in window points.

TEST(KeyboardLiftTest, IphoneMatchesTheKeyboardHeight) {
  // 393x852, home indicator 34: the bar ends at 818; a 336 keyboard (top 516) lifts it by 302, so the shell's
  // bottom inset is 34 + 302 = 336, the same as max(safe bottom, keyboard) before.
  EXPECT_EQ(KeyboardLift(852, 516, 818, 0), 302);
  EXPECT_EQ(34 + KeyboardLift(852, 516, 818, 0), 336);
}

TEST(KeyboardLiftTest, IpadFullScreenLandscape) {
  // Logged 2026-11-16: window 954 tall, keyboard top 470 (484 tall), bar on the 20 pt home-indicator inset.
  EXPECT_EQ(20 + KeyboardLift(954, 470, 934, 0), 484);
}

TEST(KeyboardLiftTest, IpadWindowAboveTheScreenBottom) {
  // A 756 tall window whose bottom is 100 pt above the display's: the guide puts the keyboard top at 656 in the window,
  // so the bar (ending at 746) goes up 90, not the keyboard's full 484.
  EXPECT_EQ(KeyboardLift(756, 656, 746, 0), 90);
}

TEST(KeyboardLiftTest, BarAlreadyAboveTheKeyboardStays) {
  EXPECT_EQ(KeyboardLift(756, 700, 640, 0), 0);
  EXPECT_EQ(KeyboardLift(756, 700, 640, 8), 0);
  EXPECT_EQ(KeyboardLift(756, 700, 695, 8), 3);
}

TEST(KeyboardLiftTest, NoKeyboardNoLiftWhateverTheGap) {
  // Hidden, floating or undocked: the guide's top is on the window's bottom edge (or below it).
  EXPECT_EQ(KeyboardLift(756, 756, 746, 0), 0);
  EXPECT_EQ(KeyboardLift(756, 756, 746, 12), 0);
  EXPECT_EQ(KeyboardLift(756, 900, 746, 12), 0);
}

TEST(KeyboardLiftTest, GapIsAdded) {
  EXPECT_EQ(KeyboardLift(852, 516, 818, 6), 308);
}

TEST(KeyboardLiftTest, BarMeasuredFromItsBoxSettlesOnTheKeyboardTop) {
  // iPad (ShellHost::MeasureKeyboardInput): the bar's bottom without a lift = its box now + the lift already applied.
  // Model of the shell: the bar ends `c` above the lifted bottom; below the 120 pt latch it also keeps the 20 pt
  // home-indicator padding inside its box. It must settle on the keyboard top within two frames and stay there.
  const int window_h = 878;
  const int safe = 20;
  const int keyboard_top = 878 - 400;
  auto bar_bottom = [&](int lift) {
    const int inset = safe + lift;
    return inset >= 120 ? window_h - inset - 6 : window_h - 6;
  };
  int lift = 0;
  for (int frame = 0; frame < 3; ++frame) {
    lift = KeyboardLift(window_h, keyboard_top, bar_bottom(lift) + lift, 0);
  }
  EXPECT_EQ(bar_bottom(lift), keyboard_top);
  EXPECT_EQ(KeyboardLift(window_h, keyboard_top, bar_bottom(lift) + lift, 0), lift);
}

// SettleBarLift: the shell is modelled as a bar whose bottom moves `per_point` pt up per pt of lift.
struct ShellModel {
  int unlifted_bottom;
  double per_point;
  int BarBottom(int lift) const { return unlifted_bottom - static_cast<int>(std::lround(lift * per_point)); }
};

TEST(SettleBarLiftTest, DockedBarLandsInOneStep) {
  const ShellModel shell{858, 1.0};
  BarLiftSettle settle;
  int lift = SettleBarLift(settle, 0, shell.BarBottom(0), 394, true);
  EXPECT_EQ(lift, 464);
  EXPECT_EQ(SettleBarLift(settle, lift, shell.BarBottom(lift), 394, false), lift);
  EXPECT_FALSE(settle.settling);
}

TEST(SettleBarLiftTest, CentredBarLandsInTwoSteps) {
  // 11-17 iPad log: an empty chat's bar at 470 went up 38 pt for a 76 pt lift; it settled at 151-152 over 8 frames.
  const ShellModel shell{470, 0.5};
  BarLiftSettle settle;
  int lift = SettleBarLift(settle, 0, shell.BarBottom(0), 394, true);
  EXPECT_EQ(lift, 76);
  lift = SettleBarLift(settle, lift, shell.BarBottom(lift), 394, false);
  EXPECT_EQ(lift, 152);
  EXPECT_EQ(shell.BarBottom(lift), 394);
  EXPECT_EQ(SettleBarLift(settle, lift, shell.BarBottom(lift), 394, false), lift);
  EXPECT_FALSE(settle.settling);
}

TEST(SettleBarLiftTest, KeystrokesWithoutANewTargetNeverMoveTheBar) {
  // Settled at 152; a keystroke frame measures the bar somewhere else (e.g. a stray reading): no new target, no move.
  BarLiftSettle settle;
  EXPECT_EQ(SettleBarLift(settle, 152, 528, 394, false), 152);
  EXPECT_EQ(SettleBarLift(settle, 152, 393, 394, false), 152);
}

TEST(SettleBarLiftTest, OneOrTwoPointsAboveTheKeyboardIsSettled) {
  BarLiftSettle settle;
  EXPECT_EQ(SettleBarLift(settle, 152, 393, 394, true), 152);
  EXPECT_FALSE(settle.settling);
  EXPECT_EQ(SettleBarLift(settle, 152, 392, 394, true), 152);
  EXPECT_NE(SettleBarLift(settle, 152, 390, 394, true), 152);
}

TEST(SettleBarLiftTest, BarThatDoesNotFollowTheLiftIsLeftAlone) {
  BarLiftSettle settle;
  int lift = SettleBarLift(settle, 0, 600, 394, true);
  EXPECT_EQ(lift, 206);
  EXPECT_EQ(SettleBarLift(settle, lift, 600, 394, false), 0);
  EXPECT_FALSE(settle.settling);
}

TEST(SettleBarLiftTest, KeyboardGoingDownLowersTheLift) {
  const ShellModel shell{858, 1.0};
  BarLiftSettle settle;
  // Settled at 464 for a keyboard top of 394; the keyboard's top moves to 520 (shorter keyboard).
  const int lift = SettleBarLift(settle, 464, shell.BarBottom(464), 520, true);
  EXPECT_EQ(lift, 338);
  EXPECT_EQ(shell.BarBottom(lift), 520);
}

TEST(KeyboardLiftTest, FieldNearTheTopIsNotLifted) {
  // A search box ending at 120 pt with the keyboard's top at 478: its own bottom is the input, so nothing moves.
  EXPECT_EQ(KeyboardLift(878, 478, 120, 0), 0);
}

TEST(KeyboardLiftTest, NeverPastTheWindowTop) {
  EXPECT_EQ(KeyboardLift(500, -40, 480, 0), 480);
  EXPECT_EQ(KeyboardLift(500, 0, 480, 30), 480);
}

} // namespace
} // namespace pbr
