#pragma once

#include "base/ui/ShellTypes.h"
#include "feature/ui/CallChromeSync.h"

#include <functional>

namespace pbr {

/**
 * Call ring / in-call chrome ports. Application fills from ShellHost.
 * Clear via BindShellCallChrome({}).
 *
 * CallController classifies chrome changes and notifies via apply_chrome_update;
 * ShellHost owns Remount / DirtyCallChrome / RequestForceFrame (not grab-bag DirtyWindow).
 */
struct ShellCallChromePorts {
  std::function<CallRingState&()> call_ring;
  std::function<CallInProgressState&()> call_in_progress;
  std::function<void(CallChromeUpdate)> apply_chrome_update;
};

class ShellHost;

ShellCallChromePorts MakeShellCallChromePorts(ShellHost& shell);

} // namespace pbr
