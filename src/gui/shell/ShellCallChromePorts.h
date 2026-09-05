#pragma once

#include "domain/ui/ShellTypes.h"
#include "gui/CallChromeSync.h"

#include <functional>

namespace pbr {

/** Presenter-owned call chrome copied into ShellHost::State on apply. */
struct CallChromeSnapshot {
  CallRingState ring;
  CallInProgressState in_progress;
};

/**
 * Call ring / in-call chrome ports. Application fills from ShellHost.
 * Clear via BindShellCallChrome({}).
 *
 * CallController owns local ring / in-call snapshots, classifies updates, and
 * pushes apply_snapshot — ports must not return mutable ShellHost::State refs.
 */
struct ShellCallChromePorts {
  /** Copy snapshot into ShellHost::State and apply Remount/Dirty from update. */
  std::function<void(const CallChromeSnapshot& snapshot, CallChromeUpdate update)> apply_snapshot;
};

class ShellHost;

ShellCallChromePorts MakeShellCallChromePorts(ShellHost& shell);

} // namespace pbr
