#pragma once

#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "common/Module.h"

namespace pbr {

/** FIFO task queue drained inline on the UI thread via RunPendingTasks(). */
class SequencedTaskRunner : public Module {
public:
  SequencedTaskRunner();
  ~SequencedTaskRunner();

  SequencedTaskRunner(const SequencedTaskRunner&) = delete;
  SequencedTaskRunner& operator=(const SequencedTaskRunner&) = delete;

  void PostTask(std::function<void()> task);
  /** Run ahead of FIFO backlog (AcceptInvite must not wait behind Prefetch/circuit). */
  void PostTaskFront(std::function<void()> task);
  void RunPendingTasks();
  void Stop();
  /** Defer execution of queued/new tasks until Resume. Does not drop posted work. */
  void Pause();
  void Resume();

  bool IsRunningOnThisThread() const;
  bool HasPendingTasks() const;

private:
  void EnqueueLocked(std::function<void()> task);
  void EnqueueFrontLocked(std::function<void()> task);
  bool DequeueOne(std::function<void()>* out);
  void RunTaskSafely(std::function<void()>& task);

  mutable std::mutex mutex_;
  std::deque<std::function<void()>> tasks_;
  bool stopped_ = false;
  bool paused_ = false;
  std::thread::id thread_id_;
};

} // namespace pbr
