#pragma once

#include <cstdint>
#include <functional>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The thread that owns call state (projects/thread-ownership t2b). Every hop inside the call stack
 * that means "continue on the calls owner" goes through here; hops that notify the GUI or the hub
 * post to UI explicitly. Retargeting the owner is a change to CallsThread.cpp only.
 *
 * After-task hooks run on the owner after every posted task — call stacks publish their UI
 * snapshot there, so the GUI always reads state as of the owner's last completed step.
 */
struct CallsThread {
  static void Post(std::function<void()> task);
  /** Ahead of queued work (answerer start, seat teardown). */
  static void PostFront(std::function<void()> task);
  static bool IsCurrent();

  using HookId = uint64_t;
  static HookId AddAfterTaskHook(std::function<void()> hook);
  static void RemoveAfterTaskHook(HookId id);
  /** Run the hooks now (owner work done inline, e.g. a bind point). */
  static void RunAfterTaskHooks();
};

} // namespace pbr
