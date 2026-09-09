#include "domain/mesh/host/MeshControlPool.h"

#include "common/Logger.h"
#include "common/PbrCompat.h"

#include <algorithm>
#include <utility>

namespace pbr {
namespace {
logging::Logger& PoolLog() {
  static logging::Logger log = logging::getLogger("MeshControlPool");
  return log;
}
} // namespace

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
  live_workers_.store(thread_count_, std::memory_order_relaxed);
  for (size_t i = 0; i < thread_count_; ++i) {
    threads_.emplace_back([this]() { WorkerMain(); });
  }
}

MeshControlPool::~MeshControlPool() { (void)Shutdown(kDefaultShutdownJoinBudget); }

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

bool MeshControlPool::Shutdown(std::chrono::milliseconds join_budget) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
      return true;
    }
    stopped_ = true;
    queue_.clear();
  }
  cv_.notify_all();

  const auto deadline = std::chrono::steady_clock::now() + join_budget;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_until(lock, deadline, [this]() {
      return live_workers_.load(std::memory_order_acquire) == 0;
    });
  }

  const bool all_exited = live_workers_.load(std::memory_order_acquire) == 0;
  bool ok = true;
  for (std::thread& t : threads_) {
    if (!t.joinable()) {
      continue;
    }
    if (all_exited) {
      t.join();
      continue;
    }
    // Worker still inside a task after abort+budget — detach and leak until process exit.
    // Destroying MeshControlPool while the worker runs would UAF; product quit exits soon after.
    PoolLog().warning << "MeshControlPool::Shutdown: worker still live after "
                      << join_budget.count() << "ms — detaching (process exit must follow)";
    t.detach();
    ok = false;
  }
  threads_.clear();
  return ok;
}

void MeshControlPool::WorkerMain() {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this]() { return stopped_ || !queue_.empty(); });
      if (stopped_ && queue_.empty()) {
        live_workers_.fetch_sub(1, std::memory_order_acq_rel);
        cv_.notify_all();
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
