#include "feature/calls/CallsThread.h"

#include "foundation/runtime/AppRuntime.h"

#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr OwnerThreadId kOwner = OwnerThreadId::MediaSessions;

/** Set while RunAndWait runs a task on UI in the owner's stead (runtime has no owner). */
thread_local int t_inline_owner = 0;

std::mutex g_hooks_mu;
std::map<CallsThread::HookId, std::shared_ptr<std::function<void()>>> g_hooks;
CallsThread::HookId g_next_hook = 1;

std::function<void()> WithHooks(std::function<void()> task) {
  return [task = std::move(task)]() {
    task();
    CallsThread::RunAfterTaskHooks();
  };
}

} // namespace

void CallsThread::Post(std::function<void()> task) {
  if (!task) {
    return;
  }
  if (AppRuntime::HasOwner(kOwner)) {
    AppRuntime::PostTo(kOwner, WithHooks(std::move(task)));
  } else {
    AppRuntime::PostUI(WithHooks(std::move(task)));
  }
}

void CallsThread::PostFront(std::function<void()> task) {
  if (!task) {
    return;
  }
  if (AppRuntime::HasOwner(kOwner)) {
    AppRuntime::PostToFront(kOwner, WithHooks(std::move(task)));
  } else {
    AppRuntime::PostUIFront(WithHooks(std::move(task)));
  }
}

bool CallsThread::IsCurrent() {
  if (t_inline_owner > 0) {
    return true;
  }
  return AppRuntime::HasOwner(kOwner) ? AppRuntime::CurrentlyOn(kOwner) : AppRuntime::CurrentlyOnUI();
}

void CallsThread::RunAndWait(const std::function<void()>& task) {
  if (!task) {
    return;
  }
  if (IsCurrent()) {
    task();  // the enclosing owner task runs the hooks
    return;
  }
  if (AppRuntime::HasOwner(kOwner)) {
    AppRuntime::RunAndWait(kOwner, [&task]() {
      task();
      RunAfterTaskHooks();
    });
    return;
  }
  // No owner: the caller stands in for the calls thread.
  ++t_inline_owner;
  struct Leave {
    ~Leave() { --t_inline_owner; }
  } leave;
  task();
  RunAfterTaskHooks();
}

CallsThread::HookId CallsThread::AddAfterTaskHook(std::function<void()> hook) {
  std::lock_guard lock(g_hooks_mu);
  const HookId id = g_next_hook++;
  g_hooks.emplace(id, std::make_shared<std::function<void()>>(std::move(hook)));
  return id;
}

void CallsThread::RemoveAfterTaskHook(const HookId id) {
  std::lock_guard lock(g_hooks_mu);
  g_hooks.erase(id);
}

void CallsThread::RunAfterTaskHooks() {
  std::vector<std::shared_ptr<std::function<void()>>> hooks;
  {
    std::lock_guard lock(g_hooks_mu);
    for (const auto& [id, hook] : g_hooks) {
      (void)id;
      hooks.push_back(hook);
    }
  }
  for (const auto& hook : hooks) {
    (*hook)();
  }
}

} // namespace pbr
