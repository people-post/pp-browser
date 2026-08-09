#pragma once

#include <functional>
#include <string>

namespace pbr {

/**
 * Unlock-gate completion ports for PinGateController (no ProfileUnlockGate*).
 * Application fills from ProfileUnlockGate. Clear via BindGateComplete({}).
 */
struct UnlockGateCompletePorts {
  std::function<void(const std::string& pin, bool create_mode)> complete_with_pin;
  std::function<void()> complete_with_default_pin;
  std::function<void()> cancel;
};

} // namespace pbr
