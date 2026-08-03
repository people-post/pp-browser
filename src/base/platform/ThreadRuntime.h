#pragma once

#include "common/WorkerPool.h"

#include <cstddef>
#include <memory>

namespace pbr {

struct ThreadRuntimeConfig {
  size_t worker_pool_threads = WorkerPool::kDefaultThreadCount;
};

/**
 * Application-owned thread budget: worker pool today; coordinator thread in t4.
 * Start from the composition root (Application, pp-node); subsystems borrow Workers().
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

  void PauseWorkers();
  void ResumeWorkers();

private:
  bool running_ = false;
  std::unique_ptr<WorkerPool> worker_pool_;
};

} // namespace pbr
