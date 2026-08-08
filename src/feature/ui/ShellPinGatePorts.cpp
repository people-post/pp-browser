#include "feature/ui/ShellPinGatePorts.h"

#include "feature/ui/ShellHost.h"

namespace pbr {

ShellPinGatePorts MakeShellPinGatePorts(ShellHost& shell) {
  ShellPinGatePorts ports;
  ports.apply_pin_gate = [&shell](const PinGateState& state) { shell.ApplyPinGateState(state); };
  ports.pin_gate_snapshot = [&shell]() { return shell.State().pin_gate; };
  ports.set_unlock_in_progress = [&shell](const bool in_progress) {
    shell.State().unlock_in_progress = in_progress;
  };
  ports.set_activity = [&shell](const bool visible, const Rml::String& message) {
    shell.SetActivity(visible, message);
  };
  ports.dirty_pin_gate = [&shell]() { shell.DirtyPinGate(); };
  ports.request_sync_layout = [&shell](const bool restore, const char* reason) {
    shell.RequestSyncLayout(restore, reason);
  };
  return ports;
}

} // namespace pbr
