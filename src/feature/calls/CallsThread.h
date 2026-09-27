#pragma once

#include "foundation/runtime/AppRuntime.h"

#include <functional>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The thread that owns call state (projects/thread-ownership t2b). Every hop inside the call stack
 * that means "continue on the calls owner" goes through here; hops that notify the GUI or the hub
 * post to UI explicitly. Retargeting the owner is a change to this file only.
 */
struct CallsThread {
  static void Post(std::function<void()> task) { AppRuntime::PostUI(std::move(task)); }
  /** Ahead of queued work (answerer start, seat teardown). */
  static void PostFront(std::function<void()> task) { AppRuntime::PostUIFront(std::move(task)); }
  static bool IsCurrent() { return AppRuntime::CurrentlyOnUI(); }
};

} // namespace pbr
