#include "foundation/runtime/RuntimeOwnerExecutor.h"

#include "foundation/runtime/AppRuntime.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace pbr {
namespace {

// A runner's executor on an owner thread (here the media-sessions owner, as CallStack's), UI without one.

RuntimeOwnerExecutor& Owner() {
  static RuntimeOwnerExecutor executor{OwnerThreadId::MediaSessions};
  return executor;
}

TEST(RuntimeOwnerExecutorTest, PostAndRunAndWaitRunOnTheMediaSessionsOwnerInOrder) {
  AppRuntime::Initialize(ManualOwnerRuntimeConfig());
  AppRuntime::InitializeUI();
  std::vector<int> order;
  bool on_owner = false;
  Owner().Post([&]() { order.push_back(1); });
  Owner().RunAndWait([&]() {
    on_owner = AppRuntime::CurrentlyOn(OwnerThreadId::MediaSessions) && Owner().IsCurrent();
    order.push_back(2);
  });
  EXPECT_TRUE(on_owner);
  EXPECT_EQ(order, (std::vector<int>{1, 2})) << "RunAndWait queues behind earlier owner work";
  EXPECT_FALSE(Owner().IsCurrent()) << "the test thread is not the owner";
  AppRuntime::ShutdownUI();
  AppRuntime::Shutdown();
}

TEST(RuntimeOwnerExecutorTest, RunAndWaitRunsAfterTaskHooksOnce) {
  AppRuntime::Initialize(ManualOwnerRuntimeConfig());
  AppRuntime::InitializeUI();
  int hooks = 0;
  const auto id = Owner().AddAfterTaskHook([&]() { ++hooks; });
  Owner().RunAndWait([]() {});
  EXPECT_EQ(hooks, 1) << "the snapshot publishes before the caller reads it";
  Owner().RemoveAfterTaskHook(id);
  AppRuntime::ShutdownUI();
  AppRuntime::Shutdown();
}

TEST(RuntimeOwnerExecutorTest, RunAndWaitIsInlineWithoutAnOwner) {
  int runs = 0;
  Owner().RunAndWait([&]() { ++runs; });
  EXPECT_EQ(runs, 1);
}

TEST(RuntimeOwnerExecutorTest, RunAndWaitRunsInlineOnceWhenTheGateDropsThePost) {
  AppRuntime::Initialize(ManualOwnerRuntimeConfig());
  AppRuntime::InitializeUI();
  ASSERT_TRUE(AppRuntime::QuiesceForTeardown(std::chrono::milliseconds(500)));
  int runs = 0;
  Owner().RunAndWait([&]() { ++runs; });
  EXPECT_EQ(runs, 1) << "a dropped post still runs the edge (teardown needs it)";
  AppRuntime::ReopenAfterTeardown();
  AppRuntime::RunUIAndOwnerTasks();
  EXPECT_EQ(runs, 1) << "and never a second time";
  AppRuntime::ShutdownUI();
  AppRuntime::Shutdown();
}

TEST(RuntimeOwnerExecutorTest, RunAndWaitBlocksUntilTheDedicatedOwnerRanIt) {
  AppRuntime::Initialize();
  AppRuntime::InitializeUI();
  std::atomic<bool> ran_on_owner{false};
  std::thread::id owner_thread;
  Owner().RunAndWait([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    owner_thread = std::this_thread::get_id();
    ran_on_owner = Owner().IsCurrent();
  });
  EXPECT_TRUE(ran_on_owner.load()) << "returned only after the owner ran the task";
  EXPECT_NE(owner_thread, std::this_thread::get_id());
  AppRuntime::ShutdownUI();
  AppRuntime::Shutdown();
}

TEST(RuntimeOwnerExecutorTest, EachExecutorRunsOnlyItsOwnHooks) {
  AppRuntime::Initialize(ManualOwnerRuntimeConfig());
  AppRuntime::InitializeUI();
  RuntimeOwnerExecutor a{OwnerThreadId::MediaSessions};
  RuntimeOwnerExecutor b{OwnerThreadId::MediaSessions};
  int a_hooks = 0;
  int b_hooks = 0;
  a.AddAfterTaskHook([&]() { ++a_hooks; });
  b.AddAfterTaskHook([&]() { ++b_hooks; });
  a.RunAndWait([]() {});
  EXPECT_EQ(a_hooks, 1);
  EXPECT_EQ(b_hooks, 0) << "two runners on one owner thread publish only their own state";
  b.Post([]() {});
  AppRuntime::RunUIAndOwnerTasks();
  EXPECT_EQ(a_hooks, 1);
  EXPECT_EQ(b_hooks, 1);
  AppRuntime::ShutdownUI();
  AppRuntime::Shutdown();
}

} // namespace
} // namespace pbr
