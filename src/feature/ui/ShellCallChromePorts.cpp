#include "feature/ui/ShellCallChromePorts.h"

#include "feature/ui/ShellHost.h"

namespace pbr {

ShellCallChromePorts MakeShellCallChromePorts(ShellHost& shell) {
  ShellCallChromePorts ports;
  ports.call_ring = [&shell]() -> CallRingState& { return shell.State().call_ring; };
  ports.call_in_progress = [&shell]() -> CallInProgressState& { return shell.State().call_in_progress; };
  ports.apply_chrome_update = [&shell](CallChromeUpdate update) { shell.ApplyCallChromeUpdate(update); };
  return ports;
}

} // namespace pbr
