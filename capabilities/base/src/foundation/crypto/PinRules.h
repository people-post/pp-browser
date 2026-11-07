#pragma once

#include <optional>
#include <string_view>

namespace pbr {

/**
 * The kind of a PIN (DEV/PLAN-pin-gate-redesign.md B): 4–8 ASCII digits → the number of digits (a digit PIN, typed on
 * the app's keypad); any other PIN of at least 4 characters → 0 (a text PIN, typed in a field; 9 digits or more count
 * here); shorter → nullopt (not a valid PIN: errors.pin.too_short).
 */
inline std::optional<int> PinDigitsOf(const std::string_view pin) {
  if (pin.size() < 4) {
    return std::nullopt;
  }
  if (pin.size() > 8) {
    return 0;
  }
  for (const char c : pin) {
    if (c < '0' || c > '9') {
      return 0;
    }
  }
  return static_cast<int>(pin.size());
}

/** A remembered `pin_digits` that means "a digit PIN of that length" (the keypad applies). */
inline bool IsDigitPinLength(const std::optional<int> digits) {
  return digits && *digits >= 4 && *digits <= 8;
}

} // namespace pbr
