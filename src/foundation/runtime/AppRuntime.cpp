#include "foundation/runtime/AppRuntime.h"

#include "foundation/runtime/ThreadRuntime.h"
#include "foundation/runtime/WorkerDispatch.h"
#include "common/Logger.h"
#include "common/SequencedTaskRunner.h"
#include "common/PbrCompat.h"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace pbr {

namespace {

std::unique_ptr<ThreadRuntime> g_thread_runtime;
bool g_testing_worker_override = false;

std::mutex g_shutdown_mu;
bool g_shutting_down = false;
uint64_t g_shutdown_generation = 0;
std::chrono::steady_clock::time_point g_shutdown_deadline{};
bool g_watchdog_armed = false;

std::mutex g_log_mu;
logging::Logger* g_log = nullptr;

// Sequenced "UI"/main mailbox — process runtime (not RmlUi). Used by GUI frame drain
// and by headless/domain reply paths (e.g. MeshDirectoryCache → PostUI).
std::unique_ptr<SequencedTaskRunner> g_ui_runner;
std::function<void()> g_ui_wake_callback;

void EnsureUIMailbox() {
  static std::mutex init_mutex;
  std::lock_guard lock(init_mutex);
  if (!g_ui_runner) {
    g_ui_runner = std::make_unique<SequencedTaskRunner>();
  }
}

/** Boundary helper: takes Logger& (do not copy Logger — LogProxy binds to `this`). */
void RunShutdownWatchdog(logging::Logger& log, uint64_t gen,
                         std::chrono::steady_clock::time_point deadline) {
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  log.error << "AppRuntime shutdown watchdog: deadline exceeded gen=" << gen
            << " — std::_Exit(0) (last resort; process was still alive)";
  std::_Exit(0);
}

} // namespace


void AppRuntime::InitLogging() {
  std::lock_guard lock(g_log_mu);
  if (g_log != nullptr) {
    return;
  }
  (void)logging::getLogger("Runtime");
  static logging::Logger instance = logging::getLogger("Runtime.AppRuntime");
  g_log = &instance;
}

logging::Logger& AppRuntime::logger() {
  InitLogging();
  return *g_log;
}

void AppRuntime::Initialize(const AppRuntimeConfig& config) {
  InitLogging();
  if (IsRunning()) {
    return;
  }
  g_thread_runtime = std::make_unique<ThreadRuntime>();
  ThreadRuntimeConfig thread_config;
  thread_config.worker_pool_threads = config.worker_pool_threads;
  g_thread_runtime->Start(thread_config);
  if (!g_testing_worker_override) {
    WorkerDispatch::Install(&g_thread_runtime->Workers());
  }
}

void AppRuntime::BeginShutdown() {
  InitLogging();
  std::chrono::steady_clock::time_point deadline;
  uint64_t gen = 0;
  bool arm_watchdog = false;
  {
    std::lock_guard lock(g_shutdown_mu);
    if (g_shutting_down) {
      return;
    }
    g_shutting_down = true;
    ++g_shutdown_generation;
    gen = g_shutdown_generation;
    g_shutdown_deadline = std::chrono::steady_clock::now() + kShutdownDeadlineBudget;
    deadline = g_shutdown_deadline;
    if (!g_watchdog_armed) {
      g_watchdog_armed = true;
      arm_watchdog = true;
    }
  }
  logger().info << "BeginShutdown gen=" << gen << " deadline_ms=" << kShutdownDeadlineBudget.count();
  if (!arm_watchdog) {
    return;
  }
  // Last resort: if graceful joins hang past the product quit budget, abandon the process.
  // Documented in THREADING.md — not a substitute for budgeted Shutdown joins.
  // Pass process-lifetime façade Logger& into the free helper (Logger is not safely copyable).
  std::thread([deadline, gen]() {
    InitLogging();
    RunShutdownWatchdog(logger(), gen, deadline);
  }).detach();
}

bool AppRuntime::IsShuttingDown() {
  std::lock_guard lock(g_shutdown_mu);
  return g_shutting_down;
}

uint64_t AppRuntime::ShutdownGeneration() {
  std::lock_guard lock(g_shutdown_mu);
  return g_shutdown_generation;
}

std::chrono::steady_clock::time_point AppRuntime::ShutdownDeadline() {
  std::lock_guard lock(g_shutdown_mu);
  return g_shutdown_deadline;
}

size_t AppRuntime::WorkerTotalQueuedCount() {
  if (!IsRunning() || !g_thread_runtime) {
    return 0;
  }
  return g_thread_runtime->Workers().TotalQueuedCount();
}

bool AppRuntime::DrainWorkersThenUI(std::chrono::milliseconds budget) {
  if (!IsRunning()) {
    RunUITasks();
    return true;
  }
  ResumeBackgroundWork();
  std::atomic<bool> done{false};
  // Critical first so we sit behind LeaveCall (Critical); then Normal behind DeclineInvite.
  PostWorker(WorkerLane::Critical, [&done]() {
    PostWorker(WorkerLane::Normal, [&done]() {
      PostUI([&done]() { done.store(true, std::memory_order_release); });
    });
  });
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!done.load(std::memory_order_acquire)) {
    RunUITasks();
    if (std::chrono::steady_clock::now() >= deadline) {
      RunUITasks();
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  RunUITasks();
  return true;
}

void AppRuntime::Shutdown() {
  if (!IsRunning()) {
    return;
  }
  // Join while WorkerDispatch still points at the pool. In-flight work (e.g. unlock →
  // EnsureMessagingReady → MeshDirectoryCache::RequestRefresh) may PostWorker; the pool
  // no-ops once stopped_. Uninstalling first asserted in WorkerDispatch::Post.
  g_thread_runtime->Shutdown();
  if (!g_testing_worker_override) {
    WorkerDispatch::Uninstall();
  }
  g_thread_runtime.reset();
}

bool AppRuntime::IsRunning() {
  return g_thread_runtime != nullptr && g_thread_runtime->IsRunning();
}

void AppRuntime::InitializeUI() {
  InitLogging();
  EnsureUIMailbox();
}

void AppRuntime::ShutdownUI() {
  g_ui_wake_callback = nullptr;
  if (g_ui_runner) {
    g_ui_runner->Stop();
    g_ui_runner.reset();
  }
}

void AppRuntime::PostUI(std::function<void()> task) {
  if (!task) {
    return;
  }
  EnsureUIMailbox();
  g_ui_runner->PostTask(std::move(task));
  if (g_ui_wake_callback) {
    g_ui_wake_callback();
  }
}

void AppRuntime::PostUIFront(std::function<void()> task) {
  if (!task) {
    return;
  }
  EnsureUIMailbox();
  g_ui_runner->PostTaskFront(std::move(task));
  if (g_ui_wake_callback) {
    g_ui_wake_callback();
  }
}

void AppRuntime::RunUITasks() {
  if (g_ui_runner) {
    g_ui_runner->RunPendingTasks();
  }
}

bool AppRuntime::HasPendingUITasks() {
  EnsureUIMailbox();
  return g_ui_runner && g_ui_runner->HasPendingTasks();
}

bool AppRuntime::CurrentlyOnUI() {
  EnsureUIMailbox();
  return g_ui_runner && g_ui_runner->IsRunningOnThisThread();
}

void AppRuntime::SetUIWakeCallback(std::function<void()> callback) {
  EnsureUIMailbox();
  g_ui_wake_callback = std::move(callback);
}

void AppRuntime::PostWorker(WorkerLane lane, std::function<void()> task) {
  // Mid-shutdown: ThreadRuntime clears running_ before joining the pool. In-flight work may
  // still PostWorker (unlock → directory refresh). No-op once !IsRunning so a nested task
  // cannot race onto another live pool thread before WorkerPool::stopped_ is set. Testing
  // overrides may post without a started ThreadRuntime.
  if (!g_testing_worker_override && !IsRunning()) {
    return;
  }
  WorkerDispatch::Post(lane, std::move(task));
}

void AppRuntime::PostWorkerCritical(std::function<void()> task) {
  PostWorker(WorkerLane::Critical, std::move(task));
}

void AppRuntime::PostWorkerNormal(std::function<void()> task) {
  PostWorker(WorkerLane::Normal, std::move(task));
}

void AppRuntime::PostWorkerBackground(std::function<void()> task) {
  PostWorker(WorkerLane::Background, std::move(task));
}

void AppRuntime::PauseWorkers() {
  if (g_thread_runtime) {
    g_thread_runtime->PauseWorkers();
  }
}

void AppRuntime::ResumeWorkers() {
  if (g_thread_runtime) {
    g_thread_runtime->ResumeWorkers();
  }
}

void AppRuntime::PauseCoordinator() {
  if (g_thread_runtime) {
    g_thread_runtime->Coordinator().Pause();
  }
}

void AppRuntime::ResumeCoordinator() {
  if (g_thread_runtime) {
    g_thread_runtime->Coordinator().Resume();
  }
}

void AppRuntime::PauseBackgroundWork() {
  PauseCoordinator();
  PauseWorkers();
}

void AppRuntime::ResumeBackgroundWork() {
  ResumeWorkers();
  ResumeCoordinator();
}

void AppRuntime::PostCoordinator(CoordinatorPriority priority, std::function<void()> task) {
  if (!IsRunning() || !task) {
    return;
  }
  g_thread_runtime->Coordinator().Post(priority, std::move(task));
}

void AppRuntime::PostCoordinatorCritical(std::function<void()> task) {
  PostCoordinator(CoordinatorPriority::Critical, std::move(task));
}

void AppRuntime::PostCoordinatorNormal(std::function<void()> task) {
  PostCoordinator(CoordinatorPriority::Normal, std::move(task));
}

void AppRuntime::PostCoordinatorBackground(std::function<void()> task) {
  PostCoordinator(CoordinatorPriority::Background, std::move(task));
}

uint64_t AppRuntime::ScheduleCoordinatorRepeating(std::chrono::milliseconds interval,
                                                  std::function<void()> fn) {
  // Mid-shutdown: ThreadRuntime clears running_ before joining workers; in-flight unlock
  // may still call StartCoordinatorTimers — no-op instead of Coordinator() assert.
  if (!IsRunning()) {
    return 0;
  }
  return g_thread_runtime->Coordinator().ScheduleRepeating(interval, std::move(fn));
}

uint64_t AppRuntime::ScheduleCoordinatorOneShot(std::chrono::milliseconds delay,
                                                std::function<void()> fn) {
  if (!IsRunning()) {
    return 0;
  }
  return g_thread_runtime->Coordinator().ScheduleOneShot(delay, std::move(fn));
}

void AppRuntime::CancelCoordinatorTimer(uint64_t timer_id) {
  if (!IsRunning() || timer_id == 0) {
    return;
  }
  g_thread_runtime->Coordinator().CancelTimer(timer_id);
}

void AppRuntime::InstallWorkerPoolForTesting(WorkerPool* pool) {
  g_testing_worker_override = true;
  WorkerDispatch::Install(pool);
}

void AppRuntime::ClearWorkerPoolForTesting() {
  WorkerDispatch::Uninstall();
  g_testing_worker_override = false;
}

} // namespace pbr
