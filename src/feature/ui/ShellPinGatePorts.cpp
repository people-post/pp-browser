#include "feature/ui/ShellPinGatePorts.h"

#include "feature/ui/ShellHost.h"

namespace pbr {

ShellPinGatePorts MakeShellPinGatePorts(ShellHost& shell) {
  ShellPinGatePorts ports;
  ports.pin_gate = [&shell]() -> PinGateState& { return shell.State().pin_gate; };
  ports.unlock_in_progress = [&shell]() -> bool& { return shell.State().unlock_in_progress; };
  ports.set_activity = [&shell](const bool visible, const Rml::String& message) {
    shell.SetActivity(visible, message);
  };
  ports.dirty_window = [&shell]() { shell.DirtyWindow(); };
  ports.request_sync_layout = [&shell](const bool restore, const char* reason) {
    shell.RequestSyncLayout(restore, reason);
  };
  return ports;
}

} // namespace pbr
