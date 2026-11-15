#include "foundation/platform/ScreenKeyboardGeometry.h"

#include <gtest/gtest.h>

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

} // namespace
} // namespace pbr
