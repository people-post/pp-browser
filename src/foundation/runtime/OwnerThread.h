#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include "common/PbrCompat.h"

namespace pbr {

/** The product's owner threads besides UI and Mesh I/O (projects/thread-ownership T001). */
enum class OwnerThreadId : uint8_t { MediaSessions = 0, Connectivity = 1 };
inline constexpr size_t kOwnerThreadCount = 2;
/** OS thread name (≤ 15 chars for pthread). */
const char* OwnerThreadName(OwnerThreadId id);

enum class OwnerThreadMode : uint8_t {
  /** Its own OS thread (product). */
  Dedicated,
  /** No thread: tasks run when the driving thread calls `RunPending` (tests, harnesses). */
  Manual,
};

/**
 * One serialized owner: a FIFO of tasks run one at a time, on a dedicated thread or — in Manual
 * mode — on whichever thread drains it. `IsCurrent()` is true only inside its tasks (and on the
 * dedicated thread). Use through `AppRuntime::PostTo` (teardown-gated), not directly.
 */
class OwnerThread {
public:
  OwnerThread(std::string name, OwnerThreadMode mode, std::function<void(const std::string&)> name_thread = {});
  ~OwnerThread();
  OwnerThread(const OwnerThread&) = delete;
  OwnerThread& operator=(const OwnerThread&) = delete;

  void Start();
  /** Stop accepting; finish the running task, drop the queue, join the thread. Idempotent. */
  void Stop();
  /** False once stopped (the task is destroyed unrun). */
  bool Post(std::function<void()> task);
  bool IsCurrent() const;
  /** Manual mode: run queued tasks (including ones they post) until empty; returns how many ran. */
  size_t RunPending();
  bool HasPending() const;
  OwnerThreadMode Mode() const { return mode_; }
  const std::string& Name() const { return name_; }

private:
  void ThreadMain();
  void RunTask(std::function<void()>& task);

  const std::string name_;
  const OwnerThreadMode mode_;
  std::function<void(const std::string&)> name_thread_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  bool started_ = false;
  bool stopped_ = false;
  std::thread thread_;
};

} // namespace pbr
