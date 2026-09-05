#pragma once

#include "common/Module.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include "common/PbrCompat.h"

namespace pbr {

enum class CoordinatorPriority { Critical, Normal, Background };

/** Dedicated joinable thread: priority mailbox + repeating/one-shot timers. Handlers must be fast. */
class CoordinatorThread : public Module {
public:
  CoordinatorThread();
  ~CoordinatorThread();

  CoordinatorThread(const CoordinatorThread&) = delete;
  CoordinatorThread& operator=(const CoordinatorThread&) = delete;

  void Start();
  void Shutdown();

  void Post(CoordinatorPriority priority, std::function<void()> task);

  /** Returns an opaque timer id (0 if not started). Repeats until cancelled. */
  uint64_t ScheduleRepeating(std::chrono::milliseconds interval, std::function<void()> fn);
  /** Returns an opaque timer id (0 if not started). Fires once. */
  uint64_t ScheduleOneShot(std::chrono::milliseconds delay, std::function<void()> fn);
  void CancelTimer(uint64_t timer_id);

  /** Defer mailbox + timer dispatch until Resume (posted work is retained). */
  void Pause();
  void Resume();

private:
  struct TimerEntry {
    uint64_t id = 0;
    std::chrono::steady_clock::time_point next_fire{};
    std::chrono::milliseconds interval{};
    bool repeat = false;
    bool cancelled = false;
    std::function<void()> fn;
  };

  void ThreadMain();
  bool DequeueOneLocked(std::function<void()>* out);
  bool HasWorkLocked() const;
  std::chrono::steady_clock::time_point NextTimerDeadlineLocked() const;
  void FireDueTimersLocked(std::chrono::steady_clock::time_point now);
  void RunTaskSafely(std::function<void()>& task);
  void EnqueueLocked(CoordinatorPriority priority, std::function<void()> task);
  void WakeCoordinatorLocked();

  std::thread thread_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> critical_queue_;
  std::deque<std::function<void()>> normal_queue_;
  std::deque<std::function<void()>> background_queue_;
  std::vector<TimerEntry> timers_;
  uint64_t next_timer_id_ = 1;
  bool started_ = false;
  bool stopped_ = false;
  bool paused_ = false;
};

} // namespace pbr
