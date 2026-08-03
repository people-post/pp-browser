#include "common/SequencedTaskRunner.h"

#include <exception>

namespace pbr {

SequencedTaskRunner::SequencedTaskRunner() {
  redirectLogger("SequencedTaskRunner");
  thread_id_ = std::this_thread::get_id();
}

SequencedTaskRunner::~SequencedTaskRunner() {
  Stop();
}

void SequencedTaskRunner::RunTaskSafely(std::function<void()>& task) {
  if (!task) {
    return;
  }
  try {
    task();
  } catch (const std::exception& e) {
    log().error << "Uncaught exception in UI task: " << e.what();
  } catch (...) {
    log().error << "Uncaught unknown exception in UI task";
  }
}

void SequencedTaskRunner::PostTask(std::function<void()> task) {
  if (!task) {
    return;
  }
  std::lock_guard lock(mutex_);
  if (stopped_) {
    return;
  }
  EnqueueLocked(std::move(task));
}

void SequencedTaskRunner::PostTaskFront(std::function<void()> task) {
  if (!task) {
    return;
  }
  std::lock_guard lock(mutex_);
  if (stopped_) {
    return;
  }
  EnqueueFrontLocked(std::move(task));
}

void SequencedTaskRunner::RunPendingTasks() {
  for (;;) {
    std::function<void()> task;
    {
      std::lock_guard lock(mutex_);
      if (paused_ || !DequeueOne(&task)) {
        break;
      }
    }
    RunTaskSafely(task);
  }
}

void SequencedTaskRunner::Pause() {
  std::lock_guard lock(mutex_);
  paused_ = true;
}

void SequencedTaskRunner::Resume() {
  std::lock_guard lock(mutex_);
  paused_ = false;
}

void SequencedTaskRunner::Stop() {
  std::lock_guard lock(mutex_);
  stopped_ = true;
  tasks_.clear();
}

bool SequencedTaskRunner::IsRunningOnThisThread() const {
  return std::this_thread::get_id() == thread_id_;
}

bool SequencedTaskRunner::HasPendingTasks() const {
  std::lock_guard lock(mutex_);
  return !tasks_.empty() && !stopped_ && !paused_;
}

void SequencedTaskRunner::EnqueueLocked(std::function<void()> task) {
  tasks_.push_back(std::move(task));
}

void SequencedTaskRunner::EnqueueFrontLocked(std::function<void()> task) {
  tasks_.push_front(std::move(task));
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
