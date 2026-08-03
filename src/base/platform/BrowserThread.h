#pragma once

#include "base/platform/PlatformRuntime.h"
#include "common/SequencedTaskRunner.h"

#include <functional>
#include <memory>

namespace pbr {

enum class BrowserThreadId { UI, IO };

class BrowserThread {
public:
  static void Initialize();
  static void Shutdown();

  static SequencedTaskRunner& Get(BrowserThreadId id);
  static void RunUITasks();
  /** True when the UI mailbox has work — ProcessEvents must not power-save-wait. */
  static bool HasPendingUITasks();

  static bool CurrentlyOn(BrowserThreadId id);

  static void PostTask(BrowserThreadId id, std::function<void()> task);
  static void PostTaskFront(BrowserThreadId id, std::function<void()> task);

  // Optional hook so UI posts break idle waits. Prefer Backend::RequestForceFrame (sets
  // force_next_frame + WakeEventLoop) — not WakeEventLoop alone — so the mailbox drains soon.
  static void SetUIWakeCallback(std::function<void()> callback);

  static void PauseIO();
  static void ResumeIO();

  template <typename Result>
  static void PostTaskAndReply(std::function<Result()> work, std::function<void(Result)> reply) {
    PostWorkerAndReplyOnUI(WorkerLane::Normal, std::move(work), std::move(reply));
  }

  /** Like PostTaskAndReply but work jumps the Normal lane queue (AcceptInvite behind Prefetch). */
  template <typename Result>
  static void PostTaskFrontAndReply(std::function<Result()> work, std::function<void(Result)> reply) {
    PostWorkerAndReplyOnUI(WorkerLane::Critical, std::move(work), std::move(reply));
  }

private:
  template <typename Result>
  static void PostWorkerAndReplyOnUI(WorkerLane lane, std::function<Result()> work,
                                     std::function<void(Result)> reply) {
    PlatformRuntime::PostWorker(lane, [work = std::move(work), reply = std::move(reply)]() mutable {
      Result result = work();
      PostTask(BrowserThreadId::UI, [reply = std::move(reply), result = std::move(result)]() mutable {
        reply(std::move(result));
      });
    });
  }

  static std::unique_ptr<SequencedTaskRunner> ui_runner_;
  static std::function<void()> ui_wake_callback_;
};

} // namespace pbr
