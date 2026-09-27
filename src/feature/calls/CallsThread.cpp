#include "feature/calls/CallsThread.h"

#include "foundation/runtime/AppRuntime.h"

#include "common/Logger.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

/** Set while RunAndWait runs a task inline in the owner's stead (no owner / dropped post). */
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

constexpr OwnerThreadId kOwner = OwnerThreadId::MediaSessions;

logging::Logger& log() {
  static logging::Logger log = logging::getLogger("Calls.Thread");
  return log;
}

/** Settles a RunAndWait: explicitly after the task ran, or when a dropped post destroys it. */
struct WaitState {
  std::mutex mu;
  std::condition_variable cv;
  bool settled = false;
  bool ran = false;

  void Settle(bool did_run) {
    {
      std::lock_guard lock(mu);
      if (settled) {
        return;
      }
      settled = true;
      ran = did_run;
    }
    cv.notify_all();
  }
};

struct DropSettle {
  explicit DropSettle(std::shared_ptr<WaitState> s) : state(std::move(s)) {}
  DropSettle(const DropSettle&) = delete;
  DropSettle& operator=(const DropSettle&) = delete;
  ~DropSettle() { state->Settle(false); }
  std::shared_ptr<WaitState> state;
};

/** The caller stands in for the owner: call-state affinity checks accept it. */
void RunInline(const std::function<void()>& task) {
  ++t_inline_owner;
  struct Leave {
    ~Leave() { --t_inline_owner; }
  } leave;
  task();
  CallsThread::RunAfterTaskHooks();
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
  if (!AppRuntime::HasOwner(kOwner)) {
    RunInline(task);
    return;
  }
  auto state = std::make_shared<WaitState>();
  auto drop = std::make_shared<DropSettle>(state);
  AppRuntime::PostTo(kOwner, [&task, state, drop]() {
    task();
    RunAfterTaskHooks();
    state->Settle(true);
  });
  drop.reset();  // only the posted task (if queued) keeps it: dropping the task settles the wait
  if (AppRuntime::OwnerThreadsManual()) {
    // Tests pump owners on the calling thread: run the queue up to our task.
    for (;;) {
      {
        std::lock_guard lock(state->mu);
        if (state->settled) {
          break;
        }
      }
      if (AppRuntime::RunOwnerTasks(kOwner) == 0) {
        break;
      }
    }
  } else {
    std::unique_lock lock(state->mu);
    while (!state->cv.wait_for(lock, std::chrono::seconds(5), [&]() { return state->settled; })) {
      log().warning << "RunAndWait: calls owner busy for >5s";
    }
  }
  bool ran = false;
  {
    std::lock_guard lock(state->mu);
    ran = state->settled && state->ran;
  }
  if (!ran) {
    RunInline(task);  // dropped (teardown gate / owner stopping): nothing else runs call state now
  }
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
