#include "base/runtime/AppRuntime.h"

#include "base/runtime/ThreadRuntime.h"
#include "base/runtime/WorkerDispatch.h"

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
  if (!g_testing_worker_override) {
    WorkerDispatch::Uninstall();
  }
  g_thread_runtime->Shutdown();
  g_thread_runtime.reset();
}

bool AppRuntime::IsRunning() {
  return g_thread_runtime != nullptr && g_thread_runtime->IsRunning();
}

void AppRuntime::PostWorker(WorkerLane lane, std::function<void()> task) {
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
  if (!g_thread_runtime || !task) {
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
  if (!g_thread_runtime) {
    return 0;
  }
  return g_thread_runtime->Coordinator().ScheduleRepeating(interval, std::move(fn));
}

uint64_t AppRuntime::ScheduleCoordinatorOneShot(std::chrono::milliseconds delay,
                                                std::function<void()> fn) {
  if (!g_thread_runtime) {
    return 0;
  }
  return g_thread_runtime->Coordinator().ScheduleOneShot(delay, std::move(fn));
}

void AppRuntime::CancelCoordinatorTimer(uint64_t timer_id) {
  if (!g_thread_runtime || timer_id == 0) {
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
