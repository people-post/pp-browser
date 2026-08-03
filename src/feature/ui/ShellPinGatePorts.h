#pragma once

#include "base/ui/ShellTypes.h"

#include <functional>

namespace pbr {

/**
 * PIN gate chrome ports — presentation state owned by ShellHost, mutated via ports.
 * Application fills from ShellHost. Clear via BindShellPinGate({}).
 */
struct ShellPinGatePorts {
  std::function<PinGateState&()> pin_gate;
  std::function<bool&()> unlock_in_progress;
  std::function<void(bool visible, const Rml::String& message)> set_activity;
  std::function<void()> dirty_window;
  std::function<void(bool restore_focus_after, const char* reason)> request_sync_layout;
};

class ShellHost;

ShellPinGatePorts MakeShellPinGatePorts(ShellHost& shell);

} // namespace pbr
