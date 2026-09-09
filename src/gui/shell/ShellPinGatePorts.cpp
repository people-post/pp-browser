#include "gui/shell/ShellPinGatePorts.h"

#include "gui/shell/ShellHost.h"

namespace pbr {

ShellPinGatePorts MakeShellPinGatePorts(ShellHost& shell) {
  ShellPinGatePorts ports;
  ports.apply_pin_gate = [&shell](const PinGateState& state) { shell.ApplyPinGateState(state); };
  ports.pin_gate_snapshot = [&shell]() { return shell.State().pin_gate; };
  ports.set_unlock_in_progress = [&shell](const bool in_progress) {
    shell.State().unlock_in_progress = in_progress;
    shell.ReconcileStartupCover();
  };
  ports.set_activity = [&shell](const bool visible, const Rml::String& message) {
    shell.SetActivity(visible, message);
  };
  ports.dirty_pin_gate = [&shell]() { shell.DirtyPinGate(); };
  ports.remount_pin_gate = [&shell]() { shell.RemountPinGateChrome(); };
  ports.settle_startup_cover = [&shell]() { shell.SettleStartupCoverArm(); };
  return ports;
}

} // namespace pbr
