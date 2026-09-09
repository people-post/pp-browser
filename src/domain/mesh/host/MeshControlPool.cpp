#include "domain/mesh/host/MeshControlPool.h"

#include <algorithm>
#include <utility>

namespace pbr {

size_t MeshControlPool::ClampThreadCount(const size_t thread_count) {
  if (thread_count < kMinThreadCount) {
    return kMinThreadCount;
  }
  if (thread_count > kMaxThreadCount) {
    return kMaxThreadCount;
  }
  return thread_count;
}

MeshControlPool::MeshControlPool(const size_t thread_count)
    : thread_count_(ClampThreadCount(thread_count)) {
  threads_.reserve(thread_count_);
  for (size_t i = 0; i < thread_count_; ++i) {
    threads_.emplace_back([this]() { WorkerMain(); });
  }
}

MeshControlPool::~MeshControlPool() { Shutdown(); }

bool MeshControlPool::IsRunning() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !stopped_;
}

void MeshControlPool::Post(std::function<void()> task) {
  if (!task) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
      return;
    }
    queue_.push_back(std::move(task));
  }
  cv_.notify_one();
}

void MeshControlPool::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
      return;
    }
    stopped_ = true;
    queue_.clear();
  }
  cv_.notify_all();
  for (std::thread& t : threads_) {
    if (t.joinable()) {
      t.join();
    }
  }
  threads_.clear();
}

void MeshControlPool::WorkerMain() {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this]() { return stopped_ || !queue_.empty(); });
      if (stopped_ && queue_.empty()) {
        return;
      }
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    if (task) {
      task();
    }
  }
}

} // namespace pbr
