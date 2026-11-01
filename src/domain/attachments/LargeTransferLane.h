#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace pbr {

/**
 * Where a large transfer (a file above the direct size) runs: one dedicated background thread for uploads and
 * one for downloads, each taking its jobs one at a time in order. A large upload or download can take minutes;
 * on the shared worker pool it would hold a worker (and, for downloads, the queue of small ones) the whole time.
 *
 * Owned by ConversationsHub and stopped by it: `RequestStop()` (shutdown begins) makes `Stopping()` true, which
 * the jobs' progress callbacks read to abort their transfer; `Shutdown()` does that and joins both threads, so
 * no job runs once the hub starts to be destroyed. Threads are started by the first job of their direction.
 * Jobs posted after the stop are dropped.
 */
class LargeTransferLane {
public:
  enum class Direction { Upload, Download };

  LargeTransferLane() = default;
  ~LargeTransferLane() { Shutdown(); }
  LargeTransferLane(const LargeTransferLane&) = delete;
  LargeTransferLane& operator=(const LargeTransferLane&) = delete;

  void Post(Direction direction, std::function<void()> job);
  bool Stopping() const { return stopping_.load(std::memory_order_acquire); }
  void RequestStop();
  void Shutdown();

private:
  struct Lane {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
  };
  void Run(Lane& lane);

  Lane upload_;
  Lane download_;
  std::atomic<bool> stopping_{false};
};

} // namespace pbr
