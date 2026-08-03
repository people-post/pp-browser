#include "feature/ui/ShellCallChromePorts.h"

#include "feature/ui/ShellHost.h"

namespace pbr {

ShellCallChromePorts MakeShellCallChromePorts(ShellHost& shell) {
  ShellCallChromePorts ports;
  ports.call_ring = [&shell]() -> CallRingState& { return shell.State().call_ring; };
  ports.call_in_progress = [&shell]() -> CallInProgressState& { return shell.State().call_in_progress; };
  ports.dirty_window = [&shell]() { shell.DirtyWindow(); };
  ports.remount_call_chrome = [&shell]() { shell.RemountCallChrome(); };
  return ports;
}

} // namespace pbr
