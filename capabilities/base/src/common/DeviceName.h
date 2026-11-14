#pragma once

#include <cstddef>
#include <string>

namespace pbr {

/** Longest device name, in Unicode code points (a combined emoji may be cut in the middle: accepted). */
inline constexpr size_t kMaxDeviceNameChars = 32;

/**
 * A device name made safe to keep and show: invalid UTF-8 gives "" (nothing is salvaged); control characters
 * (C0, DEL, C1, line and paragraph separators) are dropped; leading and trailing whitespace is trimmed; at most
 * kMaxDeviceNameChars code points are kept (cut by code point, never inside a character). "" means no name.
 * Pure: the same rules for what a person types, what the system reports and what a locker holds.
 */
std::string NormalizeDeviceName(const std::string& raw);

} // namespace pbr
