#include "common/WorkerPool.h"

#include <algorithm>
#include <exception>
#include <string>

#if defined(__ANDROID__) || defined(__linux__)
#include <pthread.h>
#endif

namespace pp {

namespace {

std::deque<std::function<void()>>* QueueForLane(WorkerLane lane,
                                                std::deque<std::function<void()>>* critical,
                                                std::deque<std::function<void()>>* normal,
                                                std::deque<std::function<void()>>* background) {
  switch (lane) {
  case WorkerLane::Critical:
    return critical;
  case WorkerLane::Normal:
    return normal;
  case WorkerLane::Background:
    return background;
  }
  return normal;
}

} // namespace

size_t WorkerPool::ClampThreadCount(const size_t thread_count) {
  if (thread_count < kMinThreadCount) {
    return kMinThreadCount;
  }
  return std::min(thread_count, kMaxThreadCount);
}

WorkerPool::WorkerPool(const size_t thread_count)
    : thread_count_(ClampThreadCount(thread_count)) {
  redirectLogger("WorkerPool");
  threads_.reserve(thread_count_);
  for (size_t i = 0; i < thread_count_; ++i) {
    threads_.emplace_back([this, i]() { WorkerMain(i); });
  }
}

WorkerPool::~WorkerPool() {
  Shutdown();
}

void WorkerPool::Post(WorkerLane lane, std::function<void()> task) {
  if (!task) {
    return;
  }

  {
    std::lock_guard lock(mutex_);
    if (stopped_) {
      return;
    }
    EnqueueLocked(lane, std::move(task));
  }
  cv_.notify_one();
}

void WorkerPool::Pause() {
  std::lock_guard lock(mutex_);
  paused_ = true;
}

void WorkerPool::Resume() {
  {
    std::lock_guard lock(mutex_);
    paused_ = false;
  }
  cv_.notify_all();
}

void WorkerPool::Shutdown() {
  {
    std::lock_guard lock(mutex_);
    if (stopped_) {
      return;
    }
    stopped_ = true;
    critical_queue_.clear();
    normal_queue_.clear();
    background_queue_.clear();
  }
  cv_.notify_all();
  for (std::thread& thread : threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  threads_.clear();
}

size_t WorkerPool::QueuedCount(const WorkerLane lane) const {
  std::lock_guard lock(mutex_);
  switch (lane) {
  case WorkerLane::Critical:
    return critical_queue_.size();
  case WorkerLane::Normal:
    return normal_queue_.size();
  case WorkerLane::Background:
    return background_queue_.size();
  }
  return 0;
}

size_t WorkerPool::TotalQueuedCount() const {
  std::lock_guard lock(mutex_);
  return critical_queue_.size() + normal_queue_.size() + background_queue_.size();
}

void WorkerPool::WorkerMain(const size_t worker_index) {
  // CRT/pthread shim — see docs/architecture/PLATFORM_CODE.md (allowlisted in common/).
#if defined(__ANDROID__) || defined(__linux__)
  const std::string name = "pp-worker-" + std::to_string(worker_index);
  pthread_setname_np(pthread_self(), name.c_str());
#else
  (void)worker_index;
#endif

  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this]() { return stopped_ || (!paused_ && HasWorkLocked()); });
      // Once Shutdown sets stopped_, never dequeue — queued work was dropped under the same lock.
      if (stopped_) {
        break;
      }
      if (paused_ || !DequeueOneLocked(&task)) {
        continue;
      }
    }
    RunTaskSafely(task);
  }
}

bool WorkerPool::DequeueOneLocked(std::function<void()>* out) {
  std::deque<std::function<void()>>* queue = nullptr;
  if (!critical_queue_.empty()) {
    queue = &critical_queue_;
  } else if (!normal_queue_.empty()) {
    queue = &normal_queue_;
  } else if (!background_queue_.empty()) {
    queue = &background_queue_;
  } else {
    return false;
  }

  *out = std::move(queue->front());
  queue->pop_front();
  return static_cast<bool>(*out);
}

bool WorkerPool::HasWorkLocked() const {
  return !critical_queue_.empty() || !normal_queue_.empty() || !background_queue_.empty();
}

void WorkerPool::EnqueueLocked(WorkerLane lane, std::function<void()> task) {
  std::deque<std::function<void()>>* const queue =
      QueueForLane(lane, &critical_queue_, &normal_queue_, &background_queue_);
  queue->push_back(std::move(task));
}

void WorkerPool::RunTaskSafely(std::function<void()>& task) {
  if (!task) {
    return;
  }
  try {
    task();
  } catch (const std::exception& e) {
    log().error << "Uncaught exception in worker task: " << e.what();
  } catch (...) {
    log().error << "Uncaught unknown exception in worker task";
  }
}

} // namespace pp
