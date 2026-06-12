#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace pbr {

// FIFO task queue. IO runner uses a dedicated thread; UI runner drains on the main thread.
class SequencedTaskRunner {
public:
  explicit SequencedTaskRunner(bool uses_dedicated_thread);
  ~SequencedTaskRunner();

  SequencedTaskRunner(const SequencedTaskRunner&) = delete;
  SequencedTaskRunner& operator=(const SequencedTaskRunner&) = delete;

  void PostTask(std::function<void()> task);
  void RunPendingTasks();
  void Stop();

  bool IsRunningOnThisThread() const;

private:
  void IOThreadMain();
  void EnqueueLocked(std::function<void()> task);
  bool DequeueOne(std::function<void()>* out);

  const bool uses_dedicated_thread_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  bool stopped_ = false;
  std::thread::id thread_id_;
};

} // namespace pbr
