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
};

class ShellHost;

ShellCallChromePorts MakeShellCallChromePorts(ShellHost& shell);

} // namespace pbr
