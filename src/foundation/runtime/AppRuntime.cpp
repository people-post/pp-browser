#include "foundation/runtime/AppRuntime.h"

#include "foundation/runtime/ThreadRuntime.h"
#include "foundation/runtime/WorkerDispatch.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::unique_ptr<ThreadRuntime> g_thread_runtime;
bool g_testing_worker_override = false;

} // namespace

void AppRuntime::Initialize(const AppRuntimeConfig& config) {
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
