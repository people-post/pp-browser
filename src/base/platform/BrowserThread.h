#pragma once

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

  static bool CurrentlyOn(BrowserThreadId id);

  static void PostTask(BrowserThreadId id, std::function<void()> task);
  static void PostTaskFront(BrowserThreadId id, std::function<void()> task);

  // Optional hook (typically Backend::WakeEventLoop) so UI posts break power-save waits.
  static void SetUIWakeCallback(std::function<void()> callback);

  static void PauseIO();
  static void ResumeIO();

  template <typename Result>
  static void PostTaskAndReply(std::function<Result()> work, std::function<void(Result)> reply) {
    PostTask(BrowserThreadId::IO, [work = std::move(work), reply = std::move(reply)]() mutable {
      Result result = work();
      PostTask(BrowserThreadId::UI, [reply = std::move(reply), result = std::move(result)]() mutable {
        reply(std::move(result));
      });
    });
  }

  /** Like PostTaskAndReply but IO work jumps the FIFO (Samsung: AcceptInvite behind Prefetch). */
  template <typename Result>
  static void PostTaskFrontAndReply(std::function<Result()> work, std::function<void(Result)> reply) {
    PostTaskFront(BrowserThreadId::IO, [work = std::move(work), reply = std::move(reply)]() mutable {
      Result result = work();
      PostTask(BrowserThreadId::UI, [reply = std::move(reply), result = std::move(result)]() mutable {
        reply(std::move(result));
      });
    });
  }

private:
  static std::unique_ptr<SequencedTaskRunner> ui_runner_;
  static std::unique_ptr<SequencedTaskRunner> io_runner_;
  static std::function<void()> ui_wake_callback_;
};

} // namespace pbr
