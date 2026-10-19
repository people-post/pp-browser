#pragma once

#include "common/WorkerPool.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

namespace pbr {

/**
 * The threads the mesh runs on, as the embedding app gives them (pp-cpp-mesh extraction,
 * docs/architecture/MESH.md): the mesh's owner lane — one serial thread for connectivity state —,
 * timers, worker lanes for CPU and disk work, and the app's main loop for results the app reads
 * there. The mesh calls these and nothing of the app's runtime.
 *
 * The app defines them: pp-browser and pp-node in `AppRuntimeMeshThreads.cpp` (AppRuntime, the
 * Connectivity owner). They behave as AppRuntime's calls of the same role do — before the runtime
 * starts too (a task for an owner not running runs in place; timers and workers do nothing).
 */
struct MeshThreads {
  using Task = std::function<void()>;

  // --- The owner lane (connectivity, reach, reachability, media plane) ---
  /** The owner thread runs (else owner work runs in place, the caller standing in for it). */
  static bool HasOwner();
  static void PostToOwner(Task task);
  /** On the owner lane: queued when it runs, else in place. */
  static void PostToOwnerOrRun(Task task);
  /** Runs `task` on the owner lane and waits for it (in place when already on it, or no owner runs). */
  static void RunOnOwnerAndWait(const Task& task);
  /** `task` on the owner lane after `delay`; a timer id for CancelTimer (0: none scheduled). */
  static uint64_t ScheduleOnOwner(std::chrono::milliseconds delay, Task task);
  /** Aborts unless on the owner lane (or no owner runs): owner-thread affinity; see PBR_MESH_ASSERT_ON_OWNER. */
  static void AssertOnOwner(const char* where);
  /** Tests: owner threads drained by hand (RunOwnerTasks) instead of running on their own. */
  static bool OwnerTasksManual();
  static size_t RunOwnerTasks();

  // --- The coordinator: one serial lane with timers ---
  static void PostCoordinator(Task task);
  static uint64_t ScheduleOneShot(std::chrono::milliseconds delay, Task task);
  /** Cancels a timer from ScheduleOneShot or ScheduleOnOwner. */
  static void CancelTimer(uint64_t timer_id);

  // --- Workers ---
  /** The app's runtime runs: worker posts are taken. */
  static bool IsRunning();
  static void PostWorker(pp::WorkerLane lane, Task task);
  /** `work` on a worker, then `reply` with its result on the app's main loop. */
  template <typename Result>
  static void PostWorkerAndReplyToApp(pp::WorkerLane lane, std::function<Result()> work,
                                      std::function<void(Result)> reply) {
    PostWorker(lane, [work = std::move(work), reply = std::move(reply)]() mutable {
      Result result = work();
      PostToApp([reply = std::move(reply), result = std::move(result)]() mutable { reply(std::move(result)); });
    });
  }

  // --- The app ---
  /** The app's main loop (pp-browser: the UI thread). */
  static void PostToApp(Task task);
  /** The app is shutting down: start nothing new. */
  static bool IsShuttingDown();
};

} // namespace pbr

#ifdef NDEBUG
#define PBR_MESH_ASSERT_ON_OWNER() ((void)0)
#else
/** Owner-lane affinity check; no-op in release builds (as PBR_ASSERT_ON_OWNER). */
#define PBR_MESH_ASSERT_ON_OWNER() ::pbr::MeshThreads::AssertOnOwner(__func__)
#endif
