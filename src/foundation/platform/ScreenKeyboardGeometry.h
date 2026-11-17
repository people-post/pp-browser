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

/** iPad: where the chat input bar's settling stands (SettleBarLift). */
struct BarLiftSettle {
  bool settling = false;
  int steps = 0;
  int prev_lift = -1; // the lift and the bar's bottom one step back; -1 = no step yet
  int prev_bottom = 0;
};

/**
 * iPad: the lift that puts the chat input bar on the keyboard's top, settled from the bar's measured bottom. Only a new
 * target (the keyboard's top, the bar's height, the bar itself, the layout or the window changed: `target_changed`)
 * starts a settling; otherwise the lift stays as it is, so typing never moves the bar. A step moves by the remaining
 * distance divided by how far the bar went per point of lift on the step before (1 for a bar docked on the shell's
 * bottom, 1/2 for one centred in an empty chat), so it lands in one step or two. Done when the bar is on the keyboard
 * or up to 2 pt above it, after 4 steps, or when the bar does not follow the lift at all (then the step is undone).
 */
inline int SettleBarLift(BarLiftSettle& s, int lift_now, int bar_bottom_now, int keyboard_top, bool target_changed) {
  constexpr int kAboveTolerance = 2;
  constexpr int kMaxSteps = 4;
  if (target_changed) {
    s = BarLiftSettle{};
    s.settling = true;
  }
  if (!s.settling) {
    return lift_now;
  }
  const int error = bar_bottom_now - keyboard_top; // > 0: the keyboard covers the bar
  if (error <= 0 && error >= -kAboveTolerance) {
    s.settling = false;
    return lift_now;
  }
  double per_point = 1.0;
  if (s.prev_lift >= 0 && s.prev_lift != lift_now) {
    per_point = static_cast<double>(s.prev_bottom - bar_bottom_now) / static_cast<double>(lift_now - s.prev_lift);
    if (per_point < 0.1) {
      // The lift did not move the bar: take the step back.
      s.settling = false;
      return s.prev_lift;
    }
    per_point = std::min(per_point, 1.5);
  }
  s.prev_lift = lift_now;
  s.prev_bottom = bar_bottom_now;
  if (++s.steps >= kMaxSteps) {
    s.settling = false;
  }
  return std::max(0, lift_now + static_cast<int>(std::ceil(static_cast<double>(error) / per_point)));
}

} // namespace pbr
