#pragma once

#include "foundation/runtime/DeferredSelf.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_set>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Runs work on the calls owner — the one thread every call object lives on. The owner (CallStack)
 * hands it to the objects it builds; they keep a CallsTasks over it and never name the thread
 * themselves (THREADING.md § Calls owner).
 */
class CallsExecutor {
public:
  using TimerId = uint64_t;

  virtual ~CallsExecutor() = default;
  /** From any thread: run `task` on the owner, after the work already queued. */
  virtual void Post(std::function<void()> task) = 0;
  /** Ahead of queued work (answerer start, seat teardown). */
  virtual void PostFront(std::function<void()> task) = 0;
  /** Run `task` on the owner after `delay`; 0 when it could not be scheduled. */
  virtual TimerId After(std::chrono::milliseconds delay, std::function<void()> task) = 0;
  virtual void Cancel(TimerId id) = 0;
  virtual bool IsCurrent() const = 0;
};

/** The product executor: the media-sessions owner (else UI), via CallsThread. */
CallsExecutor& CallsOwnerExecutor();

/**
 * One object's work on the calls owner. Everything it posts or schedules is dropped once the object
 * is gone (or DropPending ran): no per-class alive flags. Any thread may post (results arriving from
 * I/O or workers) or cancel; the tasks and timers run on the owner.
 */
class CallsTasks {
public:
  explicit CallsTasks(CallsExecutor& executor) : executor_(executor) {}
  ~CallsTasks();
  CallsTasks(const CallsTasks&) = delete;
  CallsTasks& operator=(const CallsTasks&) = delete;

  void Post(std::function<void()> task);
  void PostFront(std::function<void()> task);
  /** A timer on the owner; returns its id (0: not scheduled). */
  CallsExecutor::TimerId After(std::chrono::milliseconds delay, std::function<void()> task);
  /** Cancel `id` (no-op for 0) and reset it to 0. */
  void Cancel(CallsExecutor::TimerId& id);
  /** Drop everything queued or scheduled so far; later work runs normally. */
  void DropPending();
  bool IsCurrent() const { return executor_.IsCurrent(); }
  CallsExecutor& Executor() const { return executor_; }

private:
  CallsExecutor& executor_;
  DeferredSelf self_;
  std::mutex mu_;
  /** Timers not yet fired. */
  std::unordered_set<CallsExecutor::TimerId> timers_;
};

} // namespace pbr
