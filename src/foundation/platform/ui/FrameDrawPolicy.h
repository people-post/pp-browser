#pragma once

namespace pbr {

/**
 * Whether the main loop may draw this frame (B127). On Android the loop keeps running in the
 * background, but SDL has let go of the GL surface there (it backs the context up on pause and
 * restores it on resume): no GL call until the foreground. Other platforms keep their own paths.
 */
inline bool ShouldDrawFrame(bool is_android, bool is_foreground) {
  return !is_android || is_foreground;
}

} // namespace pbr
