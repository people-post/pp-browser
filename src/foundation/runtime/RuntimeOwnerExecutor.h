#pragma once

#include "foundation/runtime/OwnerTasks.h"
#include "foundation/runtime/OwnerThread.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The OwnerExecutor of a runner on one of the runtime's owner threads (THREADING.md § Owner
 * runners). The runner holds it and hands it down; its components never name the thread.
 *
 * Without that owner (runtime not initialized or shut down, a harness), work goes to the UI thread
 * instead; RunAndWait runs inline then, the caller standing in for the owner (IsCurrent() is true
 * for the task's duration). After-task hooks run on the owner after every task this executor runs —
 * a runner publishes its UI snapshot there, so the GUI reads state as of the owner's last step.
 * They are this executor's: two runners on one owner thread do not run each other's hooks.
 */
class RuntimeOwnerExecutor final : public OwnerExecutor {
public:
  explicit RuntimeOwnerExecutor(OwnerThreadId owner);

  void Post(std::function<void()> task) override;
  void PostFront(std::function<void()> task) override;
  /** The coordinator fires the timer; the task runs as a posted task (hooks included). */
  TimerId After(std::chrono::milliseconds delay, std::function<void()> task) override;
  void Cancel(TimerId id) override;
  bool IsCurrent() const override;
  /**
   * Inline when already on the owner, when there is no owner, or when the post is dropped
   * (teardown gate). The owner never waits on its callers, so this cannot deadlock.
   */
  void RunAndWait(const std::function<void()>& task) override;

  using HookId = uint64_t;
  HookId AddAfterTaskHook(std::function<void()> hook);
  void RemoveAfterTaskHook(HookId id);
  /** Run the hooks now (owner work done inline, e.g. a bind point). */
  void RunAfterTaskHooks();

private:
  struct Hooks {
    std::mutex mu;
    std::map<HookId, std::shared_ptr<std::function<void()>>> by_id;
    HookId next = 1;
    void Run();
  };

  /** Posted tasks hold the hooks, not the executor: one that outlives it runs no hooks of a dead runner. */
  std::function<void()> WithHooks(std::function<void()> task) const;

  const OwnerThreadId owner_;
  const std::shared_ptr<Hooks> hooks_ = std::make_shared<Hooks>();
};

} // namespace pbr
