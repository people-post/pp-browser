#include "feature/ui/shell/ShellCallChromePorts.h"

#include "feature/ui/shell/ShellHost.h"

namespace pbr {

ShellCallChromePorts MakeShellCallChromePorts(ShellHost& shell) {
  ShellCallChromePorts ports;
  ports.apply_snapshot = [&shell](const CallChromeSnapshot& snapshot, CallChromeUpdate update) {
    shell.ApplyCallChromeSnapshot(snapshot, update);
  };
  return ports;
}

} // namespace pbr
