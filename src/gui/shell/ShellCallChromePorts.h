#pragma once

#include "lib/ui/ShellTypes.h"
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
  /** The shell is in its phone layout (the full-screen call page, ShellCallChromeRml). Unset: false. */
  std::function<bool()> phone_layout;
  /** Window height in dp less the status bar and the bottom inset: the height of the call page's content. 0: unknown. */
  std::function<int()> call_page_height_dp;
};

class ShellHost;

ShellCallChromePorts MakeShellCallChromePorts(ShellHost& shell);

} // namespace pbr
