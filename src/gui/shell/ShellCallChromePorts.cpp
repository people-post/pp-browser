#include "gui/shell/ShellCallChromePorts.h"

#include "gui/shell/ShellHost.h"

namespace pbr {

ShellCallChromePorts MakeShellCallChromePorts(ShellHost& shell) {
  ShellCallChromePorts ports;
  ports.apply_snapshot = [&shell](const CallChromeSnapshot& snapshot, CallChromeUpdate update) {
    shell.ApplyCallChromeSnapshot(snapshot, update);
  };
  ports.phone_layout = [&shell]() { return shell.State().layout_mode == LayoutMode::Compact; };
  ports.call_page_height_dp = [&shell]() { return shell.CallPageHeightDp(); };
  return ports;
}

} // namespace pbr
