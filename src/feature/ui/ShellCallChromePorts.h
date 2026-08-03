#pragma once

#include "base/ui/ShellTypes.h"

#include <functional>

namespace pbr {

/**
 * Call ring / in-call chrome ports. Application fills from ShellHost.
 * Clear via BindShellCallChrome({}).
 */
struct ShellCallChromePorts {
  std::function<CallRingState&()> call_ring;
  std::function<CallInProgressState&()> call_in_progress;
  std::function<void()> dirty_window;
  /** Mount/unmount ring + in-call overlays into dedicated mounts (not full SyncLayout). */
  std::function<void()> remount_call_chrome;
};

class ShellHost;

ShellCallChromePorts MakeShellCallChromePorts(ShellHost& shell);

} // namespace pbr
