#include "base/platform/CoordinatorThread.h"

#include <algorithm>
#include <cassert>

namespace pbr {

CoordinatorThread::CoordinatorThread() = default;

CoordinatorThread::~CoordinatorThread() {
  Shutdown();
}

void CoordinatorThread::Start() {
  std::lock_guard lock(mutex_);
  if (started_ || stopped_) {
    return;
  }
  started_ = true;
  thread_ = std::thread([this]() { ThreadMain(); });
}

void CoordinatorThread::Shutdown() {
  {
    std::lock_guard lock(mutex_);
    if (!started_ || stopped_) {
      return;
    }
    stopped_ = true;
    critical_queue_.clear();
    normal_queue_.clear();
    background_queue_.clear();
    for (TimerEntry& timer : timers_) {
      timer.cancelled = true;
    }
  }
  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
  started_ = false;
  stopped_ = false;
  timers_.clear();
}

void CoordinatorThread::Post(CoordinatorPriority priority, std::function<void()> task) {
  if (!task) {
    return;
  }
  {
    std::lock_guard lock(mutex_);
    if (stopped_) {
      return;
    }
    EnqueueLocked(priority, std::move(task));
  }
  cv_.notify_one();
}

uint64_t CoordinatorThread::ScheduleRepeating(std::chrono::milliseconds interval,
                                              std::function<void()> fn) {
  if (!fn || interval.count() <= 0) {
    return 0;
  }
  std::lock_guard lock(mutex_);
  if (stopped_) {
    return 0;
  }
  const uint64_t id = next_timer_id_++;
  TimerEntry entry;
  entry.id = id;
  entry.interval = interval;
  entry.repeat = true;
  entry.next_fire = std::chrono::steady_clock::now() + interval;
  entry.fn = std::move(fn);
  timers_.push_back(std::move(entry));
  WakeCoordinatorLocked();
  return id;
}

uint64_t CoordinatorThread::ScheduleOneShot(std::chrono::milliseconds delay, std::function<void()> fn) {
  if (!fn || delay.count() < 0) {
    return 0;
  }
  std::lock_guard lock(mutex_);
  if (stopped_) {
    return 0;
  }
  const uint64_t id = next_timer_id_++;
  TimerEntry entry;
  entry.id = id;
  entry.interval = delay;
  entry.repeat = false;
  entry.next_fire = std::chrono::steady_clock::now() + delay;
  entry.fn = std::move(fn);
  timers_.push_back(std::move(entry));
  WakeCoordinatorLocked();
  return id;
}

void CoordinatorThread::CancelTimer(uint64_t timer_id) {
  if (timer_id == 0) {
    return;
  }
  std::lock_guard lock(mutex_);
  for (TimerEntry& timer : timers_) {
    if (timer.id == timer_id) {
      timer.cancelled = true;
      break;
    }
  }
}

void CoordinatorThread::ThreadMain() {
  std::unique_lock lock(mutex_);
  while (!stopped_) {
    std::function<void()> task;
    while (DequeueOneLocked(&task)) {
      lock.unlock();
      RunTaskSafely(task);
      lock.lock();
      if (stopped_) {
        break;
      }
    }

    const auto now = std::chrono::steady_clock::now();
    FireDueTimersLocked(now);
    if (stopped_) {
      break;
    }

    const auto deadline = NextTimerDeadlineLocked();
    if (deadline == std::chrono::steady_clock::time_point::max()) {
      cv_.wait(lock, [this]() { return stopped_ || HasWorkLocked(); });
      continue;
    }

    if (deadline <= now) {
      continue;
    }

    cv_.wait_until(lock, deadline, [this]() { return stopped_ || HasWorkLocked(); });
  }
}

bool CoordinatorThread::DequeueOneLocked(std::function<void()>* out) {
  assert(out != nullptr);
  if (!critical_queue_.empty()) {
    *out = std::move(critical_queue_.front());
    critical_queue_.pop_front();
    return true;
  }
  if (!normal_queue_.empty()) {
    *out = std::move(normal_queue_.front());
    normal_queue_.pop_front();
    return true;
  }
  if (!background_queue_.empty()) {
    *out = std::move(background_queue_.front());
    background_queue_.pop_front();
    return true;
  }
  return false;
}

bool CoordinatorThread::HasWorkLocked() const {
  return !critical_queue_.empty() || !normal_queue_.empty() || !background_queue_.empty();
}

std::chrono::steady_clock::time_point CoordinatorThread::NextTimerDeadlineLocked() const {
  std::chrono::steady_clock::time_point next = std::chrono::steady_clock::time_point::max();
  for (const TimerEntry& timer : timers_) {
    if (timer.cancelled) {
      continue;
    }
    next = std::min(next, timer.next_fire);
  }
  return next;
}

void CoordinatorThread::FireDueTimersLocked(std::chrono::steady_clock::time_point now) {
  for (TimerEntry& timer : timers_) {
    if (timer.cancelled || timer.next_fire > now || !timer.fn) {
      continue;
    }
    if (timer.repeat) {
      timer.next_fire = now + timer.interval;
    } else {
      timer.cancelled = true;
    }
    std::function<void()> fn = timer.fn;
    mutex_.unlock();
    RunTaskSafely(fn);
    mutex_.lock();
  }

  timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                               [](const TimerEntry& timer) { return timer.cancelled; }),
                timers_.end());
}

void CoordinatorThread::RunTaskSafely(std::function<void()>& task) {
  if (!task) {
    return;
  }
  try {
    task();
  } catch (...) {
    // Coordinator handlers must not throw; swallow to keep the thread alive.
  }
  task = nullptr;
}

void CoordinatorThread::EnqueueLocked(CoordinatorPriority priority, std::function<void()> task) {
  switch (priority) {
  case CoordinatorPriority::Critical:
    critical_queue_.push_back(std::move(task));
    break;
  case CoordinatorPriority::Normal:
    normal_queue_.push_back(std::move(task));
    break;
  case CoordinatorPriority::Background:
    background_queue_.push_back(std::move(task));
    break;
  }
}

void CoordinatorThread::WakeCoordinatorLocked() {
  cv_.notify_one();
}

} // namespace pbr
