#pragma once

#include <algorithm>
#include <cmath>

namespace pbr {

/** A rectangle in points; used for the keyboard frame and our window, both in the window's own coordinates. */
struct KeyboardRect {
  double x = 0;
  double y = 0;
  double w = 0;
  double h = 0;
};

/**
 * How far up the layout must lift to clear the keyboard: the part of `keyboard` that covers the bottom of the window
 * (0, 0, window_w, window_h), rounded up. Only a keyboard that reaches the window's bottom edge counts: a floating or
 * undocked iPad keyboard sits above it and is left alone. A keyboard that starts inside the window and ends below it
 * (the window does not touch the screen bottom) covers the window down to its edge, so the overlap is clipped to it.
 * An empty frame (hidden keyboard, hardware keyboard without its shortcut bar) or one outside the window gives 0.
 */
inline int KeyboardInsetInWindow(const KeyboardRect& keyboard, double window_w, double window_h) {
  const double left = std::max(keyboard.x, 0.0);
  const double top = std::max(keyboard.y, 0.0);
  const double right = std::min(keyboard.x + keyboard.w, window_w);
  const double bottom = std::min(keyboard.y + keyboard.h, window_h);
  if (right <= left || bottom <= top) {
    return 0;
  }
  const bool reaches_bottom = keyboard.y + keyboard.h >= window_h - 1.0;
  return reaches_bottom ? static_cast<int>(std::ceil(bottom - top)) : 0;
}

/**
 * How far the input bar must go up to sit on the keyboard (WeChat style, Kenneth 2026-11-16): its bottom edge minus
 * the keyboard's top edge plus `gap`, never below 0 and never past the window's top. Only the bar counts: whatever else
 * the keyboard covers stays covered. All values in the window's own points, y down; `keyboard_top` at or below
 * `window_h` means no keyboard over the window (hidden, floating or undocked), so no lift whatever the gap.
 */
inline int KeyboardLift(int window_h, int keyboard_top, int input_bottom, int gap) {
  if (keyboard_top >= window_h) {
    return 0;
  }
  return std::clamp(input_bottom - std::max(keyboard_top, 0) + gap, 0, std::max(input_bottom, 0));
}

} // namespace pbr
