#include "base/platform/ThreadRuntime.h"

#include "base/platform/CoordinatorThread.h"

#include <cassert>

namespace pbr {

ThreadRuntime::ThreadRuntime() = default;

ThreadRuntime::~ThreadRuntime() {
  Shutdown();
}

void ThreadRuntime::Start(const ThreadRuntimeConfig& config) {
  if (running_) {
    return;
  }
  worker_pool_ = std::make_unique<WorkerPool>(config.worker_pool_threads);
  coordinator_ = std::make_unique<CoordinatorThread>();
  coordinator_->Start();
  running_ = true;
}

void ThreadRuntime::Shutdown() {
  if (!running_) {
    return;
  }
  running_ = false;
  if (coordinator_) {
    coordinator_->Shutdown();
    coordinator_.reset();
  }
  if (worker_pool_) {
    worker_pool_->Shutdown();
    worker_pool_.reset();
  }
}

WorkerPool& ThreadRuntime::Workers() {
  assert(running_ && worker_pool_ != nullptr);
  return *worker_pool_;
}

const WorkerPool& ThreadRuntime::Workers() const {
  assert(running_ && worker_pool_ != nullptr);
  return *worker_pool_;
}

CoordinatorThread& ThreadRuntime::Coordinator() {
  assert(running_ && coordinator_ != nullptr);
  return *coordinator_;
}

const CoordinatorThread& ThreadRuntime::Coordinator() const {
  assert(running_ && coordinator_ != nullptr);
  return *coordinator_;
}

void ThreadRuntime::PauseWorkers() {
  if (worker_pool_) {
    worker_pool_->Pause();
  }
}

void ThreadRuntime::ResumeWorkers() {
  if (worker_pool_) {
    worker_pool_->Resume();
  }
}

} // namespace pbr
