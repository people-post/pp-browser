#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "common/Module.h"

namespace pbr {

// FIFO task queue. IO runner uses a dedicated thread; UI runner drains on the main thread.
class SequencedTaskRunner : public Module {
public:
  explicit SequencedTaskRunner(bool uses_dedicated_thread);
  ~SequencedTaskRunner();

  SequencedTaskRunner(const SequencedTaskRunner&) = delete;
  SequencedTaskRunner& operator=(const SequencedTaskRunner&) = delete;

  void PostTask(std::function<void()> task);
  void RunPendingTasks();
  void Stop();
  void Pause();
  void Resume();

  bool IsRunningOnThisThread() const;

private:
  void IOThreadMain();
  void EnqueueLocked(std::function<void()> task);
  bool DequeueOne(std::function<void()>* out);
  void RunTaskSafely(std::function<void()>& task);

  const bool uses_dedicated_thread_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  bool stopped_ = false;
  bool paused_ = false;
  std::thread::id thread_id_;
};

} // namespace pbr
