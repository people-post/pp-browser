#pragma once

#include <cstddef>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace pbr {

/**
 * Fixed 1–2 thread pool for Amp control waits (Connect / IoPumpUntil).
 * Owned by MeshHost — not the general AppRuntime WorkerPool.
 */
class MeshControlPool {
public:
  static constexpr size_t kMinThreadCount = 1;
  static constexpr size_t kMaxThreadCount = 2;
  static constexpr size_t kDefaultThreadCount = 2;

  explicit MeshControlPool(size_t thread_count = kDefaultThreadCount);
  ~MeshControlPool();

  MeshControlPool(const MeshControlPool&) = delete;
  MeshControlPool& operator=(const MeshControlPool&) = delete;

  void Post(std::function<void()> task);
  /** Stop accepting work, drop queued tasks, join workers (in-flight tasks still finish). */
  void Shutdown();

  size_t ThreadCount() const { return thread_count_; }
  bool IsRunning() const;

private:
  static size_t ClampThreadCount(size_t thread_count);
  void WorkerMain();

  const size_t thread_count_;
  std::vector<std::thread> threads_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> queue_;
  bool stopped_ = false;
};

} // namespace pbr
