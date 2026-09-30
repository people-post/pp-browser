#pragma once

#include "foundation/runtime/OwnerTasks.h"

#include <chrono>
#include <deque>
#include <functional>
#include <utility>

namespace pbr::test {

/**
 * An owner thread by hand for component tests: posts and timers (delay ignored) go onto the
 * test's queue, which the test drains; the draining thread is the owner.
 */
class QueueOwnerExecutor final : public OwnerExecutor {
public:
  explicit QueueOwnerExecutor(std::deque<std::function<void()>>& queue) : queue_(queue) {}

  void Post(std::function<void()> task) override { queue_.push_back(std::move(task)); }
  void PostFront(std::function<void()> task) override { queue_.push_front(std::move(task)); }
  TimerId After(std::chrono::milliseconds /*delay*/, std::function<void()> task) override {
    queue_.push_back(std::move(task));
    return ++next_;
  }
  void Cancel(TimerId /*id*/) override {}  // a cancelled timer's task is dropped by OwnerTasks
  bool IsCurrent() const override { return true; }
  void RunAndWait(const std::function<void()>& task) override { task(); }

private:
  std::deque<std::function<void()>>& queue_;
  TimerId next_ = 0;
};

} // namespace pbr::test
