#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace pbr {

/**
 * Fixed 1–2 thread pool for Amp control waits (reachability / remaining IoPumpUntil facades).
 * Owned by MeshHost — not the general AppRuntime WorkerPool.
 * Default 1 after ConnectAsync + MeshPump-driven parks; raise to 2 if control wait fan-out grows.
 */
class MeshControlPool {
public:
  static constexpr size_t kMinThreadCount = 1;
  static constexpr size_t kMaxThreadCount = 2;
  static constexpr size_t kDefaultThreadCount = 1;
  /** Soft join budget on product shutdown (abort should make workers exit sooner). */
  static constexpr std::chrono::milliseconds kDefaultShutdownJoinBudget{500};

  explicit MeshControlPool(size_t thread_count = kDefaultThreadCount);
  ~MeshControlPool();

  MeshControlPool(const MeshControlPool&) = delete;
  MeshControlPool& operator=(const MeshControlPool&) = delete;

  void Post(std::function<void()> task);
  /** Stop accepting work, drop queued tasks, join workers (in-flight tasks still finish). */
  void Shutdown() { Shutdown(kDefaultShutdownJoinBudget); }
  /**
   * Like Shutdown(), but abandon join after `join_budget` if a worker is stuck mid-task.
   * Detached workers are leaked until process exit — only safe on product quit.
   * Returns false if any worker was detached after the budget.
   */
  bool Shutdown(std::chrono::milliseconds join_budget);

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
  std::atomic<size_t> live_workers_{0};
};

} // namespace pbr
