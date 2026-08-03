#pragma once

#include "base/runtime/CoordinatorThread.h"
#include "common/WorkerDispatch.h"
#include "common/WorkerPool.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace pbr {

class ThreadRuntime;

struct AppRuntimeConfig {
  size_t worker_pool_threads = WorkerPool::kDefaultThreadCount;
};

/**
 * Process-wide application runtime: worker pool, coordinator, UI mailbox.
 * Initialize from the composition root (Application ctor, pp-node bootstrap).
 * OS adapters live in base/platform/; call PlatformServices directly when needed.
 */
class AppRuntime {
public:
  static void Initialize(const AppRuntimeConfig& config = {});
  static void Shutdown();
  static bool IsRunning();

  // --- Scheduling ---
  static void PostUI(std::function<void()> task);
  static void PostWorker(WorkerLane lane, std::function<void()> task);
  static void PostWorkerCritical(std::function<void()> task);
  static void PostWorkerNormal(std::function<void()> task);
  static void PostWorkerBackground(std::function<void()> task);

  template <typename Result>
  static void PostWorkerAndReply(WorkerLane lane, std::function<Result()> work,
                                 std::function<void(Result)> on_done) {
    WorkerDispatch::PostAndReply(lane, std::move(work), std::move(on_done));
  }

  template <typename Result>
  static void PostWorkerAndReplyOnUI(WorkerLane lane, std::function<Result()> work,
                                     std::function<void(Result)> reply) {
    PostWorker(lane, [work = std::move(work), reply = std::move(reply)]() mutable {
      Result result = work();
      PostUI([reply = std::move(reply), result = std::move(result)]() mutable {
        reply(std::move(result));
      });
    });
  }

  static void PauseWorkers();
  static void ResumeWorkers();
  static void PauseCoordinator();
  static void ResumeCoordinator();
  /** Pauses coordinator + worker pool (replaces legacy BrowserThread::PauseIO). */
  static void PauseBackgroundWork();
  /** Resumes coordinator + worker pool (replaces legacy BrowserThread::ResumeIO). */
  static void ResumeBackgroundWork();

  // --- Coordinator (orchestration mailbox + timer wheel) ---
  static void PostCoordinator(CoordinatorPriority priority, std::function<void()> task);
  static void PostCoordinatorCritical(std::function<void()> task);
  static void PostCoordinatorNormal(std::function<void()> task);
  static void PostCoordinatorBackground(std::function<void()> task);
  static uint64_t ScheduleCoordinatorRepeating(std::chrono::milliseconds interval,
                                               std::function<void()> fn);
  static uint64_t ScheduleCoordinatorOneShot(std::chrono::milliseconds delay, std::function<void()> fn);
  static void CancelCoordinatorTimer(uint64_t timer_id);

  /** Override worker dispatch for unit tests (does not start a full runtime). */
  static void InstallWorkerPoolForTesting(WorkerPool* pool);
  static void ClearWorkerPoolForTesting();
};

} // namespace pbr
