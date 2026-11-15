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

} // namespace pbr
