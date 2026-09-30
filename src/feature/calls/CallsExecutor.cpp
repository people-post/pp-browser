#include "feature/calls/CallsExecutor.h"

#include "feature/calls/CallsThread.h"
#include "foundation/runtime/AppRuntime.h"

#include <memory>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

class OwnerExecutor final : public CallsExecutor {
public:
  void Post(std::function<void()> task) override { CallsThread::Post(std::move(task)); }
  void PostFront(std::function<void()> task) override { CallsThread::PostFront(std::move(task)); }
  TimerId After(const std::chrono::milliseconds delay, std::function<void()> task) override {
    // The coordinator fires the timer; the task runs as an owner task (after-task hooks included).
    return AppRuntime::ScheduleCoordinatorOneShot(
        delay, [task = std::move(task)]() mutable { CallsThread::Post(std::move(task)); });
  }
  void Cancel(const TimerId id) override {
    if (id != 0) {
      AppRuntime::CancelCoordinatorTimer(id);
    }
  }
  bool IsCurrent() const override { return CallsThread::IsCurrent(); }
};

} // namespace

CallsExecutor& CallsOwnerExecutor() {
  static OwnerExecutor executor;
  return executor;
}

CallsTasks::~CallsTasks() {
  DropPending();
}

void CallsTasks::Post(std::function<void()> task) {
  if (task) {
    executor_.Post(self_.Bind(std::move(task)));
  }
}

void CallsTasks::PostFront(std::function<void()> task) {
  if (task) {
    executor_.PostFront(self_.Bind(std::move(task)));
  }
}

CallsExecutor::TimerId CallsTasks::After(const std::chrono::milliseconds delay, std::function<void()> task) {
  if (!task) {
    return 0;
  }
  // The id is known only after scheduling: the fired task looks it up through a shared slot.
  auto slot = std::make_shared<CallsExecutor::TimerId>(0);
  std::lock_guard<std::mutex> lock(mu_);  // held until the id is recorded: the fired task waits for it
  const CallsExecutor::TimerId id = executor_.After(delay, self_.Bind([this, slot, task = std::move(task)]() {
    {
      std::lock_guard<std::mutex> fired(mu_);
      timers_.erase(*slot);
    }
    task();
  }));
  if (id != 0) {
    *slot = id;
    timers_.insert(id);
  }
  return id;
}

void CallsTasks::Cancel(CallsExecutor::TimerId& id) {
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

void CallsTasks::DropPending() {
  self_.Invalidate();
  std::unordered_set<CallsExecutor::TimerId> timers;
  {
    std::lock_guard<std::mutex> lock(mu_);
    timers.swap(timers_);
  }
  for (const CallsExecutor::TimerId id : timers) {
    executor_.Cancel(id);
  }
}

} // namespace pbr
