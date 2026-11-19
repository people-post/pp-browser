#pragma once

#include <cmath>

namespace pbr {

/** How close the screen's brightness must still be to what we set for it to count as untouched (the OS reports it as a
 *  float that can round-trip a little off). */
inline constexpr float kScreenBrightnessSetTolerance = 0.02f;

/**
 * Whether the brightness saved before a QR page raised the screen should be written back: only when the screen is still
 * at (about) the value we set. If the person changed it meanwhile (Control Center), their choice stays.
 */
inline bool ShouldRestoreScreenBrightness(const float current, const float set_by_us) {
  return std::fabs(current - set_by_us) <= kScreenBrightnessSetTolerance;
}

} // namespace pbr
