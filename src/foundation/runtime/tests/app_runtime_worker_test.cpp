#include "foundation/runtime/AppRuntime.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace {

void WaitUntil(const std::function<bool()>& predicate, const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  FAIL() << "Timed out waiting for condition";
}

} // namespace

TEST(AppRuntimeWorkerTest, PostWorkerNormalRuns) {
  pbr::AppRuntime::Initialize();

  std::atomic<bool> ran{false};
  pbr::AppRuntime::PostWorkerNormal([&]() { ran.store(true); });

  WaitUntil([&]() { return ran.load(); }, std::chrono::milliseconds(2000));
  EXPECT_TRUE(ran.load());

  pbr::AppRuntime::Shutdown();
}

TEST(AppRuntimeWorkerTest, PostWorkerCriticalJumpsQueue) {
  pbr::AppRuntime::Initialize();

  std::mutex mu;
  std::condition_variable cv;
  bool gate_open = false;
  std::vector<std::string> order;

  pbr::AppRuntime::PostWorkerNormal([&]() {
    std::unique_lock lock(mu);
    cv.wait(lock, [&]() { return gate_open; });
    order.push_back("normal");
  });

  pbr::AppRuntime::PostWorkerCritical([&]() { order.push_back("critical"); });

  WaitUntil([&]() { return order.size() == 1; }, std::chrono::milliseconds(2000));
  ASSERT_EQ(order.size(), 1u);
  EXPECT_EQ(order.front(), "critical");

  {
    std::lock_guard lock(mu);
    gate_open = true;
  }
  cv.notify_all();

  WaitUntil([&]() { return order.size() == 2; }, std::chrono::milliseconds(2000));
  EXPECT_EQ(order[0], "critical");
  EXPECT_EQ(order[1], "normal");

  pbr::AppRuntime::Shutdown();
}

TEST(AppRuntimeWorkerTest, PauseBackgroundWorkPausesWorkerPool) {
  pbr::AppRuntime::Initialize();

  std::atomic<bool> ran{false};
  pbr::AppRuntime::PauseBackgroundWork();
  pbr::AppRuntime::PostWorkerNormal([&]() { ran.store(true); });

  // Pause queues work; it must not run before Resume (no settle sleep).
  EXPECT_FALSE(ran.load());

  pbr::AppRuntime::ResumeBackgroundWork();

  WaitUntil([&]() { return ran.load(); }, std::chrono::milliseconds(2000));
  EXPECT_TRUE(ran.load());

  pbr::AppRuntime::Shutdown();
}

// Quit during unlock/EnsureMessagingReady: in-flight work may PostWorker (e.g. directory
// refresh) while AppRuntime::Shutdown joins the pool. Must not assert, and must not run the
// nested task (PostWorker no-ops once ThreadRuntime has cleared running_).
TEST(AppRuntimeWorkerTest, ShutdownToleratesInFlightNestedPost) {
  pbr::AppRuntime::Initialize();

  std::mutex mu;
  std::condition_variable cv;
  bool worker_entered = false;
  std::atomic<bool> nested_post_survived{false};
  std::atomic<bool> nested_task_ran{false};

  pbr::AppRuntime::PostWorkerNormal([&]() {
    {
      std::lock_guard lock(mu);
      worker_entered = true;
    }
    cv.notify_all();
    // Wait until Shutdown has cleared running_ (before pool join). Posting earlier races:
    // shutdown_started can be true while the pool is still accepting work, so another
    // worker thread can run the nested task (Windows CI flake).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (pbr::AppRuntime::IsRunning() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (pbr::AppRuntime::IsRunning()) {
      return;
    }
    pbr::AppRuntime::PostWorkerNormal([&]() { nested_task_ran.store(true); });
    nested_post_survived.store(true);
  });

  {
    std::unique_lock lock(mu);
    cv.wait_for(lock, std::chrono::milliseconds(2000), [&]() { return worker_entered; });
    ASSERT_TRUE(worker_entered);
  }

  pbr::AppRuntime::Shutdown();

  EXPECT_TRUE(nested_post_survived.load());
  EXPECT_FALSE(nested_task_ran.load());
}
