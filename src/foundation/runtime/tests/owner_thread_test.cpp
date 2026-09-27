#include "foundation/runtime/AppRuntime.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// projects/thread-ownership T001 / T002: owner threads are serialized mailboxes behind the teardown
// gate — Dedicated (own OS thread, product) or Manual (drained by the caller, tests).

namespace {

using namespace std::chrono_literals;
using pbr::AppRuntime;
using pbr::AppRuntimeConfig;
using pbr::OwnerThreadId;
using pbr::OwnerThreadMode;

class OwnerThreadTest : public ::testing::Test {
protected:
  void Start(OwnerThreadMode mode) {
    AppRuntime::InitializeUI();
    AppRuntimeConfig config;
    config.owner_threads = mode;
    config.name_thread = [this](const std::string& name) {
      std::lock_guard lock(names_mu_);
      names_.push_back(name);
    };
    AppRuntime::Initialize(config);
  }
  void TearDown() override {
    AppRuntime::ReopenAfterTeardown();
    AppRuntime::Shutdown();
  }

  template <typename Pred>
  static bool WaitFor(Pred&& done, std::chrono::milliseconds budget = 2s) {
    const auto until = std::chrono::steady_clock::now() + budget;
    while (!done()) {
      if (std::chrono::steady_clock::now() >= until) {
        return false;
      }
      std::this_thread::sleep_for(1ms);
    }
    return true;
  }

  std::mutex names_mu_;
  std::vector<std::string> names_;
};

TEST_F(OwnerThreadTest, DedicatedRunsTasksInOrderOnItsOwnNamedThread) {
  Start(OwnerThreadMode::Dedicated);
  std::mutex mu;
  std::vector<int> order;
  std::atomic<bool> on_owner{true};
  std::thread::id owner_thread;
  for (int i = 0; i < 20; ++i) {
    AppRuntime::PostTo(OwnerThreadId::MediaSessions, [&, i] {
      std::lock_guard lock(mu);
      order.push_back(i);
      owner_thread = std::this_thread::get_id();
      if (!AppRuntime::CurrentlyOn(OwnerThreadId::MediaSessions) ||
          AppRuntime::CurrentlyOn(OwnerThreadId::Connectivity)) {
        on_owner = false;
      }
    });
  }
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard lock(mu);
    return order.size() == 20;
  }));
  std::lock_guard lock(mu);
  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(order[i], i);
  }
  EXPECT_TRUE(on_owner.load());
  EXPECT_NE(owner_thread, std::this_thread::get_id());
  EXPECT_FALSE(AppRuntime::CurrentlyOn(OwnerThreadId::MediaSessions)) << "the test thread is not the owner";
  std::lock_guard names_lock(names_mu_);
  EXPECT_NE(std::find(names_.begin(), names_.end(), "pp-media-sess"), names_.end());
  EXPECT_NE(std::find(names_.begin(), names_.end(), "pp-connectivity"), names_.end());
}

TEST_F(OwnerThreadTest, ManualRunsOnlyWhenDrainedAndFollowsCrossOwnerChains) {
  Start(OwnerThreadMode::Manual);
  ASSERT_TRUE(AppRuntime::OwnerThreadsManual());
  std::vector<std::string> trace;
  AppRuntime::PostTo(OwnerThreadId::MediaSessions, [&] {
    trace.push_back(AppRuntime::CurrentlyOn(OwnerThreadId::MediaSessions) ? "media" : "media-off-owner");
    AppRuntime::PostTo(OwnerThreadId::Connectivity, [&] {
      trace.push_back(AppRuntime::CurrentlyOn(OwnerThreadId::Connectivity) ? "conn" : "conn-off-owner");
      AppRuntime::PostTo(OwnerThreadId::MediaSessions, [&] { trace.push_back("media-reply"); });
    });
  });
  std::this_thread::sleep_for(20ms);
  EXPECT_TRUE(trace.empty()) << "manual owners have no thread of their own";
  EXPECT_EQ(AppRuntime::RunAllOwnerTasks(), 3u);
  EXPECT_EQ(trace, (std::vector<std::string>{"media", "conn", "media-reply"}));
  EXPECT_FALSE(AppRuntime::CurrentlyOn(OwnerThreadId::MediaSessions));
}

TEST_F(OwnerThreadTest, ScheduleOnPostsOntoTheOwnerAfterTheDelay) {
  Start(OwnerThreadMode::Dedicated);
  std::atomic<bool> on_owner{false};
  const auto posted = std::chrono::steady_clock::now();
  std::atomic<long long> waited_ms{0};
  ASSERT_NE(AppRuntime::ScheduleOn(OwnerThreadId::Connectivity, 30ms, [&] {
              waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - posted)
                              .count();
              on_owner = AppRuntime::CurrentlyOn(OwnerThreadId::Connectivity);
            }),
            0u);
  ASSERT_TRUE(WaitFor([&] { return on_owner.load(); }));
  EXPECT_GE(waited_ms.load(), 25);
}

TEST_F(OwnerThreadTest, QuiesceWaitsForRunningOwnerWorkThenDropsNewPosts) {
  Start(OwnerThreadMode::Dedicated);
  std::atomic<bool> finished{false};
  std::atomic<bool> started{false};
  AppRuntime::PostTo(OwnerThreadId::MediaSessions, [&] {
    started = true;
    std::this_thread::sleep_for(50ms);
    finished = true;
  });
  ASSERT_TRUE(WaitFor([&] { return started.load(); }));
  EXPECT_TRUE(AppRuntime::QuiesceForTeardown(2s));
  EXPECT_TRUE(finished.load()) << "quiesce returned while an owner task was still running";
  std::atomic<bool> late{false};
  AppRuntime::PostTo(OwnerThreadId::MediaSessions, [&] { late = true; });
  std::this_thread::sleep_for(30ms);
  EXPECT_FALSE(late.load()) << "posts after quiesce must no-op";
}

TEST_F(OwnerThreadTest, QuiescePumpsManualOwners) {
  Start(OwnerThreadMode::Manual);
  std::atomic<bool> ran{false};
  AppRuntime::PostTo(OwnerThreadId::Connectivity, [&] { ran = true; });
  EXPECT_TRUE(AppRuntime::QuiesceForTeardown(2s));
  EXPECT_TRUE(ran.load());
}

TEST_F(OwnerThreadTest, DrainWorkersThenUISitsBehindOwnerQueues) {
  Start(OwnerThreadMode::Dedicated);
  std::atomic<bool> owner_done{false};
  AppRuntime::PostTo(OwnerThreadId::MediaSessions, [&] {
    std::this_thread::sleep_for(40ms);
    owner_done = true;
  });
  EXPECT_TRUE(AppRuntime::DrainWorkersThenUI(2s));
  EXPECT_TRUE(owner_done.load());
}

// A task a stopped owner drops unrun must still settle the teardown gate: before, the next quiesce
// in the process waited out its whole budget for a task that never came.
TEST_F(OwnerThreadTest, DroppedPostsDoNotStallTheNextQuiesce) {
  Start(OwnerThreadMode::Manual);
  AppRuntime::PostTo(OwnerThreadId::MediaSessions, [] {});
  AppRuntime::Shutdown();  // owner stops with the task still queued
  AppRuntime::Initialize();
  const auto began = std::chrono::steady_clock::now();
  EXPECT_TRUE(AppRuntime::QuiesceForTeardown(1s));
  EXPECT_LT(std::chrono::steady_clock::now() - began, 500ms);
}

TEST_F(OwnerThreadTest, PostsBeforeInitializeOrAfterShutdownAreDropped) {
  Start(OwnerThreadMode::Manual);
  AppRuntime::Shutdown();
  bool ran = false;
  AppRuntime::PostTo(OwnerThreadId::MediaSessions, [&] { ran = true; });
  EXPECT_EQ(AppRuntime::RunAllOwnerTasks(), 0u);
  EXPECT_FALSE(ran);
  AppRuntime::Initialize();
}

#ifndef NDEBUG
TEST_F(OwnerThreadTest, AffinityAssertAbortsOffTheOwner) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        Start(OwnerThreadMode::Dedicated);
        PBR_ASSERT_ON_OWNER(OwnerThreadId::MediaSessions);
      },
      "affinity violated");
}

TEST_F(OwnerThreadTest, AffinityAssertPassesOnTheOwner) {
  Start(OwnerThreadMode::Manual);
  bool reached = false;
  AppRuntime::PostTo(OwnerThreadId::MediaSessions, [&] {
    PBR_ASSERT_ON_OWNER(OwnerThreadId::MediaSessions);
    reached = true;
  });
  AppRuntime::RunAllOwnerTasks();
  EXPECT_TRUE(reached);
}
#endif

} // namespace
