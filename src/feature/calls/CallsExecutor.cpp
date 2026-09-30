#include "feature/calls/CallsExecutor.h"

#include "feature/calls/CallsThread.h"
#include "foundation/runtime/AppRuntime.h"

#include <memory>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

class CallsThreadExecutor final : public OwnerExecutor {
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
  void RunAndWait(const std::function<void()>& task) override { CallsThread::RunAndWait(task); }
};

} // namespace

OwnerExecutor& CallsOwnerExecutor() {
  static CallsThreadExecutor executor;
  return executor;
}

} // namespace pbr
