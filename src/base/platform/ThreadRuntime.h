#pragma once

#include "common/WorkerPool.h"

#include <cstddef>
#include <memory>

namespace pbr {

class CoordinatorThread;

struct ThreadRuntimeConfig {
  size_t worker_pool_threads = WorkerPool::kDefaultThreadCount;
};

/**
 * Application-owned thread budget: worker pool + coordinator thread.
 * Start from the composition root (Application, pp-node); subsystems borrow via PlatformRuntime.
 */
class ThreadRuntime {
public:
  ThreadRuntime();
  ~ThreadRuntime();

  ThreadRuntime(const ThreadRuntime&) = delete;
  ThreadRuntime& operator=(const ThreadRuntime&) = delete;

  void Start(const ThreadRuntimeConfig& config = {});
  void Shutdown();

  bool IsRunning() const { return running_; }

  WorkerPool& Workers();
  const WorkerPool& Workers() const;

  CoordinatorThread& Coordinator();
  const CoordinatorThread& Coordinator() const;

  void PauseWorkers();
  void ResumeWorkers();

private:
  bool running_ = false;
  std::unique_ptr<WorkerPool> worker_pool_;
  std::unique_ptr<CoordinatorThread> coordinator_;
};

} // namespace pbr
