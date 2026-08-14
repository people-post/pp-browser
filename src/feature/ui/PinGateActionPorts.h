#pragma once

#include <functional>

namespace pbr {

/**
 * PIN gate action ports for ShellHost (no PinGateController*).
 * Application fills from PinGateController. Clear via BindPinGateActions({}).
 */
struct PinGateActionPorts {
  std::function<void()> on_submit;
  std::function<void()> on_cancel;
  std::function<void()> on_set_pin;
  std::function<void()> on_use_default;
  std::function<void()> on_identity_new;
  std::function<void()> on_identity_link;
};

} // namespace pbr
