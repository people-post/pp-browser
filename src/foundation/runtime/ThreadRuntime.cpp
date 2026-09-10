#include "foundation/runtime/ThreadRuntime.h"

#include "foundation/runtime/CoordinatorThread.h"
#include "common/Logger.h"

#include <cassert>
#include "common/PbrCompat.h"

namespace pbr {
namespace {
logging::Logger& ThreadRuntimeLog() {
  static logging::Logger log = logging::getLogger("ThreadRuntime");
  return log;
}
} // namespace

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
    if (!coordinator_->Shutdown(CoordinatorThread::kDefaultShutdownJoinBudget)) {
      ThreadRuntimeLog().warning
          << "ThreadRuntime::Shutdown: coordinator join budget exceeded — leaking until process exit";
      (void)coordinator_.release();
    } else {
      coordinator_.reset();
    }
  }
  if (worker_pool_) {
    if (!worker_pool_->Shutdown(WorkerPool::kDefaultShutdownJoinBudget)) {
      ThreadRuntimeLog().warning
          << "ThreadRuntime::Shutdown: WorkerPool join budget exceeded — leaking until process exit";
      (void)worker_pool_.release();
    } else {
      worker_pool_.reset();
    }
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
