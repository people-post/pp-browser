#include "platform/SequencedTaskRunner.h"

namespace pbr {

SequencedTaskRunner::SequencedTaskRunner(const bool uses_dedicated_thread)
    : uses_dedicated_thread_(uses_dedicated_thread) {
  if (uses_dedicated_thread_) {
    thread_ = std::thread([this]() { IOThreadMain(); });
    {
      std::lock_guard lock(mutex_);
      thread_id_ = thread_.get_id();
    }
  } else {
    thread_id_ = std::this_thread::get_id();
  }
}

SequencedTaskRunner::~SequencedTaskRunner() {
  Stop();
}

void SequencedTaskRunner::PostTask(std::function<void()> task) {
  if (!task) {
    return;
  }

  if (!uses_dedicated_thread_) {
    std::lock_guard lock(mutex_);
    if (stopped_) {
      return;
    }
    EnqueueLocked(std::move(task));
    return;
  }

  {
    std::lock_guard lock(mutex_);
    if (stopped_) {
      return;
    }
    EnqueueLocked(std::move(task));
  }
  cv_.notify_one();
}

void SequencedTaskRunner::RunPendingTasks() {
  if (uses_dedicated_thread_) {
    return;
  }

  for (;;) {
    std::function<void()> task;
    {
      std::lock_guard lock(mutex_);
      if (!DequeueOne(&task)) {
        break;
      }
    }
    task();
  }
}

void SequencedTaskRunner::Stop() {
  if (uses_dedicated_thread_) {
    {
      std::lock_guard lock(mutex_);
      stopped_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
    return;
  }

  std::lock_guard lock(mutex_);
  stopped_ = true;
  tasks_.clear();
}

bool SequencedTaskRunner::IsRunningOnThisThread() const {
  return std::this_thread::get_id() == thread_id_;
}

void SequencedTaskRunner::IOThreadMain() {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this]() { return stopped_ || !tasks_.empty(); });
      if (stopped_ && tasks_.empty()) {
        break;
      }
      if (!DequeueOne(&task)) {
        continue;
      }
    }
    task();
  }
}

void SequencedTaskRunner::EnqueueLocked(std::function<void()> task) {
  tasks_.push_back(std::move(task));
}

bool SequencedTaskRunner::DequeueOne(std::function<void()>* out) {
  if (tasks_.empty()) {
    return false;
  }
  *out = std::move(tasks_.front());
  tasks_.pop_front();
  return true;
}

} // namespace pbr
