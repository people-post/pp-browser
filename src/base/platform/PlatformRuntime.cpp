#include "base/platform/PlatformRuntime.h"

#include "base/platform/ThreadRuntime.h"
#include "common/WorkerDispatch.h"

namespace pbr {

namespace {

std::unique_ptr<ThreadRuntime> g_thread_runtime;
bool g_testing_worker_override = false;

} // namespace

void PlatformRuntime::Initialize(const PlatformRuntimeConfig& config) {
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

void PlatformRuntime::Shutdown() {
  if (!IsRunning()) {
    return;
  }
  if (!g_testing_worker_override) {
    WorkerDispatch::Uninstall();
  }
  g_thread_runtime->Shutdown();
  g_thread_runtime.reset();
}

bool PlatformRuntime::IsRunning() {
  return g_thread_runtime != nullptr && g_thread_runtime->IsRunning();
}

void PlatformRuntime::PostWorker(WorkerLane lane, std::function<void()> task) {
  WorkerDispatch::Post(lane, std::move(task));
}

void PlatformRuntime::PostWorkerCritical(std::function<void()> task) {
  PostWorker(WorkerLane::Critical, std::move(task));
}

void PlatformRuntime::PostWorkerNormal(std::function<void()> task) {
  PostWorker(WorkerLane::Normal, std::move(task));
}

void PlatformRuntime::PostWorkerBackground(std::function<void()> task) {
  PostWorker(WorkerLane::Background, std::move(task));
}

void PlatformRuntime::PauseWorkers() {
  if (g_thread_runtime) {
    g_thread_runtime->PauseWorkers();
  }
}

void PlatformRuntime::ResumeWorkers() {
  if (g_thread_runtime) {
    g_thread_runtime->ResumeWorkers();
  }
}

void PlatformRuntime::InstallWorkerPoolForTesting(WorkerPool* pool) {
  g_testing_worker_override = true;
  WorkerDispatch::Install(pool);
}

void PlatformRuntime::ClearWorkerPoolForTesting() {
  WorkerDispatch::Uninstall();
  g_testing_worker_override = false;
}

} // namespace pbr
