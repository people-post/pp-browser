#pragma once

#include "common/WorkerDispatch.h"
#include "common/WorkerPool.h"

#include <cstddef>
#include <functional>
#include <memory>

namespace Rml {
class FileInterface;
}

namespace pbr {

class IAssetLocator;
class ILocalNotifier;
class IPathProvider;
class ThreadRuntime;

struct PlatformRuntimeConfig {
  size_t worker_pool_threads = WorkerPool::kDefaultThreadCount;
};

/**
 * Unified process-wide platform facade: worker scheduling, platform service access.
 * Initialize from the composition root (Application ctor, pp-node bootstrap).
 */
class PlatformRuntime {
public:
  static void Initialize(const PlatformRuntimeConfig& config = {});
  static void Shutdown();
  static bool IsRunning();

  /** Idempotent; safe from Bootstrap before messaging init. */
  static void EnsurePlatformServices();

  // --- Scheduling ---
  static void PostUI(std::function<void()> task);
  static void PostWorker(WorkerLane lane, std::function<void()> task);
  static void PostWorkerCritical(std::function<void()> task);
  static void PostWorkerNormal(std::function<void()> task);
  static void PostWorkerBackground(std::function<void()> task);

  template <typename Result>
  static void PostWorkerAndReply(WorkerLane lane, std::function<Result()> work,
                                 std::function<void(Result)> on_done) {
    WorkerDispatch::PostAndReply(lane, std::move(work), std::move(on_done));
  }

  template <typename Result>
  static void PostWorkerAndReplyOnUI(WorkerLane lane, std::function<Result()> work,
                                     std::function<void(Result)> reply) {
    PostWorker(lane, [work = std::move(work), reply = std::move(reply)]() mutable {
      Result result = work();
      PostUI([reply = std::move(reply), result = std::move(result)]() mutable {
        reply(std::move(result));
      });
    });
  }

  static void PauseWorkers();
  static void ResumeWorkers();

  // --- Platform capabilities ---
  static IPathProvider& Paths();
  static IAssetLocator& Assets();
  static ILocalNotifier& Notifier();
  static Rml::FileInterface* PackagedFileInterface();

  /** Override worker dispatch for unit tests (does not start a full runtime). */
  static void InstallWorkerPoolForTesting(WorkerPool* pool);
  static void ClearWorkerPoolForTesting();
};

} // namespace pbr
