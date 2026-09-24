#pragma once

#include "foundation/runtime/CoordinatorThread.h"
#include "foundation/runtime/WorkerDispatch.h"
#include "common/Logger.h"
#include "common/WorkerPool.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include "common/PbrCompat.h"

namespace pbr {

class ThreadRuntime;

struct AppRuntimeConfig {
  size_t worker_pool_threads = WorkerPool::kDefaultThreadCount;
};

/**
 * Process-wide application runtime: worker pool, coordinator, UI mailbox.
 * Initialize from the composition root (Application ctor, pp-node bootstrap).
 * OS adapters live in foundation/platform/; call PlatformHooks directly when needed.
 */
class AppRuntime {
public:
  /** Soft process-exit budget after BeginShutdown (watchdog last resort). */
  static constexpr std::chrono::milliseconds kShutdownDeadlineBudget{3000};

  static void Initialize(const AppRuntimeConfig& config = {});
  static void Shutdown();
  static bool IsRunning();

  /**
   * Bind `Runtime.AppRuntime` (and parent `Runtime`). Idempotent.
   * Called from Initialize; also safe from tests / early BeginShutdown.
   * See docs/architecture/LOGGING.md.
   */
  static void InitLogging();
  static logging::Logger& logger();

  /**
   * Mark product quit: bumps generation, records now+3s deadline, arms watchdog `_Exit`.
   * Idempotent. Call from Backend::RequestExit (after HideWindow) and Application::Shutdown.
   */
  static void BeginShutdown();
  static bool IsShuttingDown();
  static uint64_t ShutdownGeneration();
  static std::chrono::steady_clock::time_point ShutdownDeadline();

  /** Queued worker tasks across lanes (0 if runtime not running). */
  static size_t WorkerTotalQueuedCount();

  /**
   * Wait until Critical then Normal worker lanes have drained past a barrier and the UI
   * mailbox has run the barrier callback. Used by CallStack::Shutdown and call tests so
   * LeaveCall/DeclineInvite workers finish before CallSessionManager is destroyed.
   * @return true if the barrier completed within budget.
   */
  static bool DrainWorkersThenUI(std::chrono::milliseconds budget = std::chrono::milliseconds(2000));

  // --- Teardown gate (docs/architecture/THREADING.md § Teardown quiesce) ---
  /**
   * Call before freeing objects that runtime tasks may reference (quit, profile reset).
   * Draining: queued work still runs and running tasks may post continuations, but new work
   * from outside (timer fires, mesh threads, fresh posts) is dropped; pumps the UI mailbox when
   * called on the UI thread; waits until no gated work is pending or running (bounded).
   * Then Closed: every queued/new post, UI task and one-shot timer no-ops.
   * @return false if work was still running at the budget — do not free what it may touch.
   */
  static bool QuiesceForTeardown(std::chrono::milliseconds budget);
  /** After teardown that keeps the process (profile reset): accept work again. Work posted
   * before the quiesce stays dead (epoch); repeating timers resume. */
  static void ReopenAfterTeardown();
  /** True between QuiesceForTeardown and ReopenAfterTeardown. */
  static bool IsTeardownQuiesced();

  // --- Main/UI mailbox (runtime_core; GUI drains each frame, headless lazy-inits) ---
  static void InitializeUI();
  static void ShutdownUI();
  static void PostUI(std::function<void()> task);
  static void PostUIFront(std::function<void()> task);
  static void RunUITasks();
  /** True when the UI mailbox has work — ProcessEvents must not power-save-wait. */
  static bool HasPendingUITasks();
  static bool CurrentlyOnUI();
  // Prefer Backend::RequestForceFrame (force_next_frame + WakeEventLoop) — not WakeEventLoop alone.
  static void SetUIWakeCallback(std::function<void()> callback);

  // --- Worker pool ---
  static void PostWorker(WorkerLane lane, std::function<void()> task);
  static void PostWorkerCritical(std::function<void()> task);
  static void PostWorkerNormal(std::function<void()> task);
  static void PostWorkerBackground(std::function<void()> task);

  template <typename Result>
  static void PostWorkerAndReply(WorkerLane lane, std::function<Result()> work,
                                 std::function<void(Result)> on_done) {
    // Through PostWorker so the teardown gate applies.
    PostWorker(lane, [work = std::move(work), on_done = std::move(on_done)]() mutable {
      Result result = work();
      if (on_done) {
        on_done(std::move(result));
      }
    });
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
  /** Pauses coordinator + worker pool (background / low-power). */
  static void PauseBackgroundWork();
  /** Resumes coordinator + worker pool. */
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
