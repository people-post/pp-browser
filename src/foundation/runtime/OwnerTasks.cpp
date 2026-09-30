#include "foundation/runtime/OwnerTasks.h"

#include <memory>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

OwnerTasks::~OwnerTasks() {
  DropPending();
}

void OwnerTasks::Post(std::function<void()> task) {
  if (task) {
    executor_.Post(self_.Bind(std::move(task)));
  }
}

void OwnerTasks::PostFront(std::function<void()> task) {
  if (task) {
    executor_.PostFront(self_.Bind(std::move(task)));
  }
}

OwnerExecutor::TimerId OwnerTasks::After(const std::chrono::milliseconds delay, std::function<void()> task) {
  if (!task) {
    return 0;
  }
  // The id is known only after scheduling: the fired task looks it up through a shared slot.
  auto slot = std::make_shared<OwnerExecutor::TimerId>(0);
  std::lock_guard<std::mutex> lock(mu_);  // held until the id is recorded: the fired task waits for it
  const OwnerExecutor::TimerId id = executor_.After(delay, self_.Bind([this, slot, task = std::move(task)]() {
    {
      std::lock_guard<std::mutex> fired(mu_);
      if (timers_.erase(*slot) == 0) {
        return;  // cancelled after the executor had already queued it
      }
    }
    task();
  }));
  if (id != 0) {
    *slot = id;
    timers_.insert(id);
  }
  return id;
}

void OwnerTasks::Cancel(OwnerExecutor::TimerId& id) {
  if (id == 0) {
    return;
  }
  executor_.Cancel(id);
  {
    std::lock_guard<std::mutex> lock(mu_);
    timers_.erase(id);
  }
  id = 0;
}

void OwnerTasks::DropPending() {
  self_.Invalidate();
  std::unordered_set<OwnerExecutor::TimerId> timers;
  {
    std::lock_guard<std::mutex> lock(mu_);
    timers.swap(timers_);
  }
  for (const OwnerExecutor::TimerId id : timers) {
    executor_.Cancel(id);
  }
}

} // namespace pbr
