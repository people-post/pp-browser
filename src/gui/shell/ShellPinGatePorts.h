#pragma once

#include "domain/ui/ShellTypes.h"

#include <functional>

namespace pbr {

/**
 * PIN gate chrome ports — presenter owns PinGateState; shell receives apply-only copies.
 * Application fills from ShellHost. Clear via BindShellPinGate({}).
 *
 * Ports must not return mutable ShellHost::State references.
 */
struct ShellPinGatePorts {
  std::function<void(const PinGateState& state)> apply_pin_gate;
  /** Read-only copy of shell-bound PIN fields (data-value writes into ShellHost). */
  std::function<PinGateState()> pin_gate_snapshot;
  std::function<void(bool in_progress)> set_unlock_in_progress;
  std::function<void(bool visible, const Rml::String& message)> set_activity;
  /** PIN gate + unlock_in_progress bindings (not grab-bag DirtyWindow). */
  std::function<void()> dirty_pin_gate;
  /** Presence remount into #shell-pin-gate-mount (not full SyncLayout). */
  std::function<void()> remount_pin_gate;
};

class ShellHost;

ShellPinGatePorts MakeShellPinGatePorts(ShellHost& shell);

} // namespace pbr
