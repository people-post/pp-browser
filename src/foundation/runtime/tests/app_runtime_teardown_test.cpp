#include "foundation/runtime/AppRuntime.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

// THREADING.md § Teardown quiesce: queued work drains, continuations run, new outside work is
// dropped, running work is waited for, and everything no-ops once Closed until Reopen.

namespace {

using namespace std::chrono_literals;
using pbr::AppRuntime;

class AppRuntimeTeardownTest : public ::testing::Test {
protected:
  void SetUp() override {
    AppRuntime::InitializeUI();
    AppRuntime::Initialize();
  }
  void TearDown() override {
    AppRuntime::ReopenAfterTeardown();
    AppRuntime::Shutdown();
  }

  static void SleepPumpingUI(std::chrono::milliseconds span) {
    const auto until = std::chrono::steady_clock::now() + span;
    while (std::chrono::steady_clock::now() < until) {
      AppRuntime::RunUITasks();
      std::this_thread::sleep_for(2ms);
    }
  }
};

TEST_F(AppRuntimeTeardownTest, QueuedWorkAndContinuationsDrainThenPostsDrop) {
  std::atomic<int> ran{0};
  std::atomic<bool> continuation{false};
  std::atomic<bool> ui_ran{false};
  AppRuntime::PostWorkerNormal([&] {
    std::this_thread::sleep_for(30ms);
    ++ran;
    // Follow-up from a running task (e.g. call_leave send) must still run while draining.
    AppRuntime::PostWorkerCritical([&] { continuation = true; });
  });
  AppRuntime::PostUI([&] { ui_ran = true; });

  EXPECT_TRUE(AppRuntime::QuiesceForTeardown(2s));
  EXPECT_EQ(ran.load(), 1);
  EXPECT_TRUE(continuation.load());
  EXPECT_TRUE(ui_ran.load()) << "UI mailbox pumped when quiescing on the UI thread";
  EXPECT_TRUE(AppRuntime::IsTeardownQuiesced());

  std::atomic<bool> late{false};
  AppRuntime::PostWorkerNormal([&] { late = true; });
  AppRuntime::PostUI([&] { late = true; });
  AppRuntime::PostCoordinatorNormal([&] { late = true; });
  SleepPumpingUI(50ms);
  EXPECT_FALSE(late.load()) << "posts after quiesce must no-op";
}

TEST_F(AppRuntimeTeardownTest, WaitsForRunningTaskAndReportsTimeout) {
  std::atomic<bool> release{false};
  std::atomic<bool> started{false};
  std::atomic<bool> finished{false};
  AppRuntime::PostWorkerNormal([&] {
    started = true;
    while (!release) {
      std::this_thread::sleep_for(1ms);
    }
    finished = true;
  });
  while (!started) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(AppRuntime::QuiesceForTeardown(30ms)) << "running task past budget must be reported";
  release = true;
  while (!finished) {  // task captures locals — let it return before they go out of scope
    std::this_thread::sleep_for(1ms);
  }
}

TEST_F(AppRuntimeTeardownTest, OutsideWorkDroppedWhileDraining) {
  std::atomic<bool> release{false};
  std::atomic<bool> started{false};
  std::atomic<bool> outside{false};
  AppRuntime::PostWorkerNormal([&] {
    started = true;
    while (!release) {
      std::this_thread::sleep_for(1ms);
    }
  });
  while (!started) {
    std::this_thread::yield();
  }
  std::thread quiesce([] { (void)AppRuntime::QuiesceForTeardown(2s); });
  while (!AppRuntime::IsTeardownQuiesced()) {
    std::this_thread::yield();
  }
  // A thread outside any runtime task (mesh pump, timer fire) posting now is new work.
  std::thread([&] { AppRuntime::PostWorkerNormal([&] { outside = true; }); }).join();
  release = true;
  quiesce.join();
  std::this_thread::sleep_for(30ms);
  EXPECT_FALSE(outside.load());
}

TEST_F(AppRuntimeTeardownTest, ReopenKeepsStaleOneShotsDeadAndResumesRepeating) {
  std::atomic<bool> one_shot{false};
  std::atomic<int> ticks{0};
  (void)AppRuntime::ScheduleCoordinatorOneShot(80ms, [&] { one_shot = true; });
  const uint64_t repeating = AppRuntime::ScheduleCoordinatorRepeating(20ms, [&] { ++ticks; });

  ASSERT_TRUE(AppRuntime::QuiesceForTeardown(1s));
  const int ticks_at_close = ticks.load();
  std::this_thread::sleep_for(60ms);
  EXPECT_EQ(ticks.load(), ticks_at_close) << "timers do not fire while closed";

  AppRuntime::ReopenAfterTeardown();
  EXPECT_FALSE(AppRuntime::IsTeardownQuiesced());
  std::this_thread::sleep_for(150ms);
  EXPECT_FALSE(one_shot.load()) << "one-shot armed before teardown must stay dead (epoch)";
  EXPECT_GT(ticks.load(), ticks_at_close) << "repeating timers (GUI) resume after reopen";
  AppRuntime::CancelCoordinatorTimer(repeating);

  std::atomic<bool> fresh{false};
  AppRuntime::PostWorkerNormal([&] { fresh = true; });
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!fresh && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_TRUE(fresh.load()) << "new work runs after reopen";
}

TEST_F(AppRuntimeTeardownTest, DrainWorkersThenUIIsNoOpOnceQuiesced) {
  ASSERT_TRUE(AppRuntime::QuiesceForTeardown(1s));
  const auto start = std::chrono::steady_clock::now();
  EXPECT_TRUE(AppRuntime::DrainWorkersThenUI(2s));
  EXPECT_LT(std::chrono::steady_clock::now() - start, 200ms) << "must not wait on dropped barrier posts";
}

} // namespace
