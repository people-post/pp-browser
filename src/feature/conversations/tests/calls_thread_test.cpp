#include "feature/calls/CallsThread.h"

#include "foundation/runtime/AppRuntime.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace pbr {
namespace {

// t2b-3: the calls owner is the media-sessions owner thread once the runtime has one.

TEST(CallsThreadTest, PostAndRunAndWaitRunOnTheMediaSessionsOwnerInOrder) {
  AppRuntime::Initialize(ManualOwnerRuntimeConfig());
  AppRuntime::InitializeUI();
  std::vector<int> order;
  bool on_owner = false;
  CallsThread::Post([&]() { order.push_back(1); });
  CallsThread::RunAndWait([&]() {
    on_owner = AppRuntime::CurrentlyOn(OwnerThreadId::MediaSessions) && CallsThread::IsCurrent();
    order.push_back(2);
  });
  EXPECT_TRUE(on_owner);
  EXPECT_EQ(order, (std::vector<int>{1, 2})) << "RunAndWait queues behind earlier owner work";
  EXPECT_FALSE(CallsThread::IsCurrent()) << "the test thread is not the owner";
  AppRuntime::ShutdownUI();
  AppRuntime::Shutdown();
}

TEST(CallsThreadTest, RunAndWaitRunsAfterTaskHooksOnce) {
  AppRuntime::Initialize(ManualOwnerRuntimeConfig());
  AppRuntime::InitializeUI();
  int hooks = 0;
  const auto id = CallsThread::AddAfterTaskHook([&]() { ++hooks; });
  CallsThread::RunAndWait([]() {});
  EXPECT_EQ(hooks, 1) << "the snapshot publishes before the caller reads it";
  CallsThread::RemoveAfterTaskHook(id);
  AppRuntime::ShutdownUI();
  AppRuntime::Shutdown();
}

TEST(CallsThreadTest, RunAndWaitIsInlineWithoutAnOwner) {
  int runs = 0;
  CallsThread::RunAndWait([&]() { ++runs; });
  EXPECT_EQ(runs, 1);
}

TEST(CallsThreadTest, RunAndWaitRunsInlineOnceWhenTheGateDropsThePost) {
  AppRuntime::Initialize(ManualOwnerRuntimeConfig());
  AppRuntime::InitializeUI();
  ASSERT_TRUE(AppRuntime::QuiesceForTeardown(std::chrono::milliseconds(500)));
  int runs = 0;
  CallsThread::RunAndWait([&]() { ++runs; });
  EXPECT_EQ(runs, 1) << "a dropped post still runs the edge (teardown needs it)";
  AppRuntime::ReopenAfterTeardown();
  AppRuntime::RunUIAndOwnerTasks();
  EXPECT_EQ(runs, 1) << "and never a second time";
  AppRuntime::ShutdownUI();
  AppRuntime::Shutdown();
}

TEST(CallsThreadTest, RunAndWaitBlocksUntilTheDedicatedOwnerRanIt) {
  AppRuntime::Initialize();
  AppRuntime::InitializeUI();
  std::atomic<bool> ran_on_owner{false};
  std::thread::id owner_thread;
  CallsThread::RunAndWait([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    owner_thread = std::this_thread::get_id();
    ran_on_owner = CallsThread::IsCurrent();
  });
  EXPECT_TRUE(ran_on_owner.load()) << "returned only after the owner ran the task";
  EXPECT_NE(owner_thread, std::this_thread::get_id());
  AppRuntime::ShutdownUI();
  AppRuntime::Shutdown();
}

} // namespace
} // namespace pbr
