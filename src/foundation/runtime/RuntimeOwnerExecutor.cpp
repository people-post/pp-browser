#include "foundation/runtime/RuntimeOwnerExecutor.h"

#include "foundation/runtime/AppRuntime.h"

#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

/** Per owner: set while RunAndWait runs a task here in the owner's stead (the runtime has none). */
thread_local int t_standing_in[kOwnerThreadCount] = {};

} // namespace

RuntimeOwnerExecutor::RuntimeOwnerExecutor(const OwnerThreadId owner) : owner_(owner) {}

void RuntimeOwnerExecutor::Hooks::Run() {
  std::vector<std::shared_ptr<std::function<void()>>> snapshot;
  {
    std::lock_guard lock(mu);
    for (const auto& [id, hook] : by_id) {
      (void)id;
      snapshot.push_back(hook);
    }
  }
  for (const auto& hook : snapshot) {
    (*hook)();
  }
}

std::function<void()> RuntimeOwnerExecutor::WithHooks(std::function<void()> task) const {
  return [task = std::move(task), hooks = hooks_]() {
    task();
    hooks->Run();
  };
}

void RuntimeOwnerExecutor::Post(std::function<void()> task) {
  if (!task) {
    return;
  }
  if (AppRuntime::HasOwner(owner_)) {
    AppRuntime::PostTo(owner_, WithHooks(std::move(task)));
  } else {
    AppRuntime::PostUI(WithHooks(std::move(task)));
  }
}

void RuntimeOwnerExecutor::PostFront(std::function<void()> task) {
  if (!task) {
    return;
  }
  if (AppRuntime::HasOwner(owner_)) {
    AppRuntime::PostToFront(owner_, WithHooks(std::move(task)));
  } else {
    AppRuntime::PostUIFront(WithHooks(std::move(task)));
  }
}

OwnerExecutor::TimerId RuntimeOwnerExecutor::After(const std::chrono::milliseconds delay,
                                                   std::function<void()> task) {
  // The coordinator outlives every runner; the task re-enters through Post with this executor's hooks.
  return AppRuntime::ScheduleCoordinatorOneShot(
      delay, [owner = owner_, task = WithHooks(std::move(task))]() mutable {
        if (AppRuntime::HasOwner(owner)) {
          AppRuntime::PostTo(owner, std::move(task));
        } else {
          AppRuntime::PostUI(std::move(task));
        }
      });
}

void RuntimeOwnerExecutor::Cancel(const TimerId id) {
  if (id != 0) {
    AppRuntime::CancelCoordinatorTimer(id);
  }
}

bool RuntimeOwnerExecutor::IsCurrent() const {
  if (t_standing_in[static_cast<size_t>(owner_)] > 0) {
    return true;
  }
  return AppRuntime::HasOwner(owner_) ? AppRuntime::CurrentlyOn(owner_) : AppRuntime::CurrentlyOnUI();
}

void RuntimeOwnerExecutor::RunAndWait(const std::function<void()>& task) {
  if (!task) {
    return;
  }
  if (IsCurrent()) {
    task();  // the enclosing owner task runs the hooks
    return;
  }
  if (AppRuntime::HasOwner(owner_)) {
    AppRuntime::RunAndWait(owner_, [&task, hooks = hooks_]() {
      task();
      hooks->Run();
    });
    return;
  }
  // No owner: the caller stands in for it.
  int& standing_in = t_standing_in[static_cast<size_t>(owner_)];
  ++standing_in;
  struct Leave {
    int& count;
    ~Leave() { --count; }
  } leave{standing_in};
  task();
  hooks_->Run();
}

RuntimeOwnerExecutor::HookId RuntimeOwnerExecutor::AddAfterTaskHook(std::function<void()> hook) {
  std::lock_guard lock(hooks_->mu);
  const HookId id = hooks_->next++;
  hooks_->by_id.emplace(id, std::make_shared<std::function<void()>>(std::move(hook)));
  return id;
}

void RuntimeOwnerExecutor::RemoveAfterTaskHook(const HookId id) {
  std::lock_guard lock(hooks_->mu);
  hooks_->by_id.erase(id);
}

void RuntimeOwnerExecutor::RunAfterTaskHooks() {
  hooks_->Run();
}

} // namespace pbr
