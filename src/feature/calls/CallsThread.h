#pragma once

#include <cstdint>
#include <functional>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The thread that owns call state (projects/thread-ownership t2b): the media-sessions owner
 * (`pp-media-sess`) once the runtime has one, else UI (runtime not initialized / shut down). Every
 * hop inside the call stack that means "continue on the calls owner" goes through here; hops that
 * notify the GUI or the hub post to UI explicitly.
 *
 * After-task hooks run on the owner after every posted task — call stacks publish their UI
 * snapshot there, so the GUI always reads state as of the owner's last completed step.
 */
struct CallsThread {
  static void Post(std::function<void()> task);
  /** Ahead of queued work (answerer start, seat teardown). */
  static void PostFront(std::function<void()> task);
  static bool IsCurrent();
  /**
   * Run `task` on the owner and wait for it (the hub's lifecycle edges: build / reset / mesh stop /
   * shutdown). Inline when already on the owner, when there is no owner, or when the post is
   * dropped (teardown gate) — the caller then stands in for the owner (`IsCurrent()` is true for
   * the task's duration). The owner never waits on its callers, so this cannot deadlock.
   */
  static void RunAndWait(const std::function<void()>& task);

  using HookId = uint64_t;
  static HookId AddAfterTaskHook(std::function<void()> hook);
  static void RemoveAfterTaskHook(HookId id);
  /** Run the hooks now (owner work done inline, e.g. a bind point). */
  static void RunAfterTaskHooks();
};

} // namespace pbr
