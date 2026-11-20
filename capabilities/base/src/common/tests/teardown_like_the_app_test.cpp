// Tests free long-lived owners the way the app does (THREADING.md § Teardown quiesce): what an owner posted raw
// `this` for runs while it is alive or not at all, and the next test starts with an open runtime.
#include "common/tests/TeardownLikeTheApp.h"

#include "common/runtime/AppRuntime.h"

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

namespace pbr::test {
namespace {

using pp::AppRuntime;
using pp::AppRuntimeConfig;

/** An owner that posts raw `this`, as the hub does, and records what ran against it and whether it was alive. */
struct Posting {
  struct Log {
    int runs = 0;
    int runs_after_free = 0;
    bool freed = false;
  };
  explicit Posting(Log& log) : log(log) {}
  ~Posting() {
    log.freed = true;
    if (post_on_free) {
      PostUI(); // a post made on the way down (a Shutdown inside a destructor)
    }
  }
  void PostUI() {
    AppRuntime::PostUI([this]() { Ran(); });
  }
  void Ran() {
    if (log.freed) {
      ++log.runs_after_free; // only reached against freed memory: what these tests rule out
    }
    ++log.runs;
  }
  Log& log;
  bool post_on_free = true;
};

TEST(TeardownLikeTheAppTest, WhatTheOwnerQueuedRunsBeforeItIsFreed) {
  Posting::Log log;
  {
    TestOwner<Posting> owner(log);
    owner->PostUI();
    owner->PostUI();
    EXPECT_EQ(log.runs, 0) << "posted, not run";
  }
  EXPECT_TRUE(log.freed);
  EXPECT_EQ(log.runs, 2) << "the quiesce ran both while the owner was alive";
  EXPECT_EQ(log.runs_after_free, 0);
}

TEST(TeardownLikeTheAppTest, APostMadeOnTheWayDownNeverRunsAndTheNextTestStartsOpen) {
  Posting::Log log;
  {
    TestOwner<Posting> owner(log);
  }
  EXPECT_TRUE(log.freed);
  EXPECT_FALSE(AppRuntime::IsTeardownQuiesced()) << "reopened for the next test";
  EXPECT_FALSE(AppRuntime::HasPendingUITasks()) << "the closed gate dropped the destructor's post";
  AppRuntime::RunUITasks();
  EXPECT_EQ(log.runs, 0);

  bool ran = false;
  AppRuntime::PostUI([&ran]() { ran = true; });
  AppRuntime::RunUITasks();
  EXPECT_TRUE(ran) << "a post after the free runs";
}

TEST(TeardownLikeTheAppTest, FreeLikeTheAppFreesUniquePtrsInOrderAndLeavesThemEmpty) {
  Posting::Log first_log;
  Posting::Log second_log;
  auto first = std::make_unique<Posting>(first_log);
  auto second = std::make_unique<Posting>(second_log);
  first->PostUI();
  second->PostUI();
  EXPECT_TRUE(FreeLikeTheApp(first, second));
  EXPECT_EQ(first, nullptr);
  EXPECT_EQ(second, nullptr);
  EXPECT_EQ(first_log.runs, 1);
  EXPECT_EQ(second_log.runs, 1) << "one quiesce for both, before either goes";
  EXPECT_EQ(first_log.runs_after_free + second_log.runs_after_free, 0);
  EXPECT_TRUE(FreeLikeTheApp(first)) << "nothing to free";
}

// Work still running at the budget: the test that made it fails, and the owner is leaked, not freed under it.
TEST(TeardownLikeTheAppTest, WorkStillRunningAtTheBudgetFailsTheTestAndKeepsTheOwner) {
  AppRuntime::Initialize(AppRuntimeConfig{});
  Posting::Log log;
  auto owner = std::make_unique<Posting>(log);
  Posting* const kept = owner.get();
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  std::atomic<bool> started{false};
  AppRuntime::PostWorkerNormal([kept, released, &started]() {
    started = true;
    released.wait();
    (void)kept->log.runs; // touches the owner: it must still be there
  });
  while (!started) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  bool freed = true;
  EXPECT_NONFATAL_FAILURE(freed = FreeLikeTheAppWithin(std::chrono::milliseconds(50), owner),
                          "still pending or running");
  EXPECT_FALSE(freed);
  EXPECT_EQ(owner, nullptr);
  EXPECT_FALSE(log.freed) << "leaked, as the app leaks messaging when its quiesce runs out";
  EXPECT_FALSE(AppRuntime::IsTeardownQuiesced()) << "reopened all the same";
  release.set_value();
  AppRuntime::Shutdown();
  kept->post_on_free = false;
  delete kept; // the test's own cleanup; the app leaves it to the process exit
}

} // namespace
} // namespace pbr::test
