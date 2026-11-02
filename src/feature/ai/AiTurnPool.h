#pragma once

#include "common/WorkerPool.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Threads of their own for AI turns (DEV/PLAN-ai-concurrent-turns.md 2b). A streamed turn holds its thread for the
 * whole answer; on the app's worker pool (four threads for all background work) a few answers at once would leave
 * nothing for configuring, storing images or the inbox.
 *
 * Not behind the runtime's teardown gate: whoever owns the turns stops them and waits here itself (AgentSessions).
 * Any thread.
 */
class AiTurnPool {
public:
  explicit AiTurnPool(size_t threads);
  /** Shutdown with the default budget. */
  ~AiTurnPool();

  AiTurnPool(const AiTurnPool&) = delete;
  AiTurnPool& operator=(const AiTurnPool&) = delete;

  /** Dropped after Shutdown. */
  void Post(std::function<void()> task);
  size_t Threads() const { return threads_; }
  /** Tasks posted and not yet finished (queued or running). */
  size_t Outstanding() const;
  /** A task posted now would start at once. */
  bool HasFreeThread() const { return Outstanding() < threads_; }
  /** True once no task is queued or running; false when `budget` ran out first. Never from a pool thread. */
  bool WaitIdle(std::chrono::milliseconds budget);
  /**
   * Drops the queued tasks and waits up to `budget` for the running ones; a task still running then is left to
   * finish on its own (the process is quitting). Idempotent; never from a pool thread.
   */
  void Shutdown(std::chrono::milliseconds budget = WorkerPool::kDefaultShutdownJoinBudget);

private:
  struct Counter {
    std::mutex mu;
    std::condition_variable cv;
    size_t count = 0;
  };

  const size_t threads_;
  std::shared_ptr<Counter> outstanding_ = std::make_shared<Counter>();
  WorkerPool pool_;
};

} // namespace pbr
