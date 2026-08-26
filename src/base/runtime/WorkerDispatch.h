#pragma once

#include "common/WorkerPool.h"

#include <functional>

namespace pbr {

/** Process-wide worker pool dispatch; installed by AppRuntime at bootstrap. */
class WorkerDispatch {
public:
  static void Install(WorkerPool* pool);
  static void Uninstall();
  static bool IsInstalled();

  static void Post(WorkerLane lane, std::function<void()> task);

  template <typename Result>
  static void PostAndReply(WorkerLane lane, std::function<Result()> work,
                           std::function<void(Result)> on_done);
};

template <typename Result>
void WorkerDispatch::PostAndReply(WorkerLane lane, std::function<Result()> work,
                                  std::function<void(Result)> on_done) {
  Post(lane, [work = std::move(work), on_done = std::move(on_done)]() mutable {
    Result result = work();
    if (on_done) {
      on_done(std::move(result));
    }
  });
}

} // namespace pbr
