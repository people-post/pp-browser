#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/**
 * Unlock-gate completion ports for PinGateController (no ProfileUnlockGate*).
 * Application fills from ProfileUnlockGate. Clear via BindGateComplete({}).
 */
struct UnlockGateCompletePorts {
  std::function<void(const std::string& pin, bool create_mode)> complete_with_pin;
  std::function<void()> complete_with_default_pin;
  std::function<void(const std::string& pin, const std::string& bundle_json, bool set_default_pin)>
      complete_link_device;
  /**
   * Restore from a recovery phrase (M023) under `pin` for this device, then unlock; `saved_copies`: saved copies of
   * the account the user picked (exported locker files' text).
   */
  std::function<void(const std::string& pin, const std::string& phrase, bool set_default_pin,
                     const std::vector<std::string>& saved_copies)>
      complete_phrase_restore;
  /**
   * Let the user pick a saved copy of their account: `picked(name, text)` on the UI thread — an empty name when
   * nothing was picked, no text when the file could not be read.
   */
  std::function<void(std::function<void(std::string name, std::optional<std::string> text)> picked)> pick_saved_copy;
  std::function<void()> cancel;
};

} // namespace pbr
