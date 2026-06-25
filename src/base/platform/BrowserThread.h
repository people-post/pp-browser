#pragma once

#include "platform/SequencedTaskRunner.h"

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

private:
  static std::unique_ptr<SequencedTaskRunner> ui_runner_;
  static std::unique_ptr<SequencedTaskRunner> io_runner_;
};

} // namespace pbr
