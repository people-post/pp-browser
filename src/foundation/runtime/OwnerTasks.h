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
 * Runs work on one owner thread. A runner (the root that owns a component tree on that thread, e.g.
 * CallStack) holds it; its components never name the thread (THREADING.md § Owner runners).
 */
class OwnerExecutor {
public:
  using TimerId = uint64_t;

  virtual ~OwnerExecutor() = default;
  /** From any thread: run `task` on the owner, after the work already queued. */
  virtual void Post(std::function<void()> task) = 0;
  /** Ahead of queued work. */
  virtual void PostFront(std::function<void()> task) = 0;
  /** Run `task` on the owner after `delay`; 0 when it could not be scheduled. */
  virtual TimerId After(std::chrono::milliseconds delay, std::function<void()> task) = 0;
  virtual void Cancel(TimerId id) = 0;
  virtual bool IsCurrent() const = 0;
  /**
   * Run `task` on the owner and wait (a runner's lifecycle edges: build / teardown). Inline when
   * already there; the owner never waits on its callers, so this cannot deadlock.
   */
  virtual void RunAndWait(const std::function<void()>& task) = 0;
};

/**
 * One object's work on an owner thread. Everything it posts or schedules is dropped once the object
 * is gone (or DropPending ran): no per-class alive flags. Any thread may post (results arriving from
 * I/O or workers) or cancel; the tasks and timers run on the owner.
 */
class OwnerTasks {
public:
  explicit OwnerTasks(OwnerExecutor& executor) : executor_(executor) {}
  ~OwnerTasks();
  OwnerTasks(const OwnerTasks&) = delete;
  OwnerTasks& operator=(const OwnerTasks&) = delete;

  void Post(std::function<void()> task);
  void PostFront(std::function<void()> task);
  /** A timer on the owner; returns its id (0: not scheduled). */
  OwnerExecutor::TimerId After(std::chrono::milliseconds delay, std::function<void()> task);
  /** Cancel `id` (no-op for 0) and reset it to 0. */
  void Cancel(OwnerExecutor::TimerId& id);
  /** Drop everything queued or scheduled so far; later work runs normally. */
  void DropPending();
  bool IsCurrent() const { return executor_.IsCurrent(); }
  OwnerExecutor& Executor() const { return executor_; }

private:
  OwnerExecutor& executor_;
  DeferredSelf self_;
  std::mutex mu_;
  /** Timers not yet fired. */
  std::unordered_set<OwnerExecutor::TimerId> timers_;
};

} // namespace pbr
