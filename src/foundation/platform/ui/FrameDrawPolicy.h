#pragma once

namespace pbr {

/**
 * Whether the main loop may draw this frame (B127). On phones the loop keeps running in the
 * background, but the GL surface is not usable there (Android: SDL backs the context up on pause;
 * iOS: GL is not allowed once the app resigns active): no GL call until the foreground. SDL maps
 * iOS willResignActive to WILL_ENTER_BACKGROUND, so a pulled-down notification center counts too.
 * Desktops keep their own paths.
 */
inline bool ShouldDrawFrame(bool is_mobile, bool is_foreground) {
  return !is_mobile || is_foreground;
}

} // namespace pbr
