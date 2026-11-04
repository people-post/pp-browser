// ToolRegistry::Execute with several AI turns at once (DEV/PLAN-ai-concurrent-turns.md 2b): app-state tools one at a
// time, UI-thread tools on the UI thread, and a stopped turn never waits out the lock or the UI thread.
#include "lib/ai/ToolRegistry.h"

#include "common/runtime/AppRuntime.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include "common/PbrCompat.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

/** Spins until `done` holds or `limit` passes; true when it held. */
template <typename Pred> bool WaitFor(Pred done, const Clock::duration limit = 2s) {
  const auto until = Clock::now() + limit;
  while (!done()) {
    if (Clock::now() >= until) {
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

class ToolRegistryWaitTest : public ::testing::Test {
protected:
  void SetUp() override {
    pbr::AppRuntime::Initialize(); // this thread is the UI thread; it is pumped only where a test says so
    Add("ui_tool", {.ui_thread = true}, [this] { ++ui_runs_; });
    Add("slow_app", {.app_state = true}, [this] {
      inside_slow_ = true;
      WaitFor([this] { return release_.load(); }, 5s);
      inside_slow_ = false;
    });
    Add("other_app", {.app_state = true}, [this] { ++other_runs_; });
    Add("network", {}, [this] {
      ++in_network_;
      WaitFor([this] { return in_network_.load() >= 2; }); // both callers inside at once, or the limit
      int seen = max_network_.load();
      const int now = in_network_.load();
      while (now > seen && !max_network_.compare_exchange_weak(seen, now)) {
      }
    });
  }
  void TearDown() override {
    release_ = true;
    pbr::AppRuntime::Shutdown();
  }

  struct Flags {
    bool app_state = false;
    bool ui_thread = false;
  };
  void Add(const std::string& name, const Flags flags, std::function<void()> body) {
    pbr::ToolMeta meta;
    meta.app_state = flags.app_state;
    meta.ui_thread = flags.ui_thread;
    registry_.Register(pbr::MakeTool(pbr::ToolDefinition{name, name, pbr::Object{}}, meta,
                                     [body = std::move(body)](const pbr::Object&) -> pbr::Roe<std::string> {
                                       body();
                                       return std::string("{}");
                                     }));
  }

  pbr::ToolRegistry registry_;
  std::atomic<int> ui_runs_{0};
  std::atomic<int> other_runs_{0};
  std::atomic<bool> inside_slow_{false};
  std::atomic<bool> release_{false};
  std::atomic<int> in_network_{0};
  std::atomic<int> max_network_{0};
};

TEST_F(ToolRegistryWaitTest, AStoppedTurnStopsWaitingForTheUiThread) {
  std::atomic<bool> cancel{false};
  std::optional<pbr::Roe<std::string>> result;
  const auto started = Clock::now();
  std::thread turn([&] { result = registry_.Execute("ui_tool", pbr::Object{}, &cancel); });
  std::this_thread::sleep_for(50ms); // the UI thread (this one) is not pumped
  cancel = true;
  turn.join();
  EXPECT_LT(Clock::now() - started, 1s) << "a stop does not wait out the 5 s";
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(*result);
  pbr::AppRuntime::RunUITasks();
  EXPECT_EQ(ui_runs_.load(), 0) << "a tool given up is never run later";
}

TEST_F(ToolRegistryWaitTest, AUiToolRunsOnTheUiThread) {
  std::optional<pbr::Roe<std::string>> result;
  std::thread turn([&] { result = registry_.Execute("ui_tool", pbr::Object{}); });
  ASSERT_TRUE(WaitFor([&] {
    pbr::AppRuntime::RunUITasks();
    return ui_runs_.load() == 1;
  }));
  turn.join();
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(*result);
}

TEST_F(ToolRegistryWaitTest, AStoppedTurnStopsWaitingForAnotherAppAction) {
  std::thread first([&] { (void)registry_.Execute("slow_app", pbr::Object{}); });
  ASSERT_TRUE(WaitFor([&] { return inside_slow_.load(); }));

  std::atomic<bool> cancel{false};
  std::optional<pbr::Roe<std::string>> result;
  const auto started = Clock::now();
  std::thread second([&] { result = registry_.Execute("other_app", pbr::Object{}, &cancel); });
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(other_runs_.load(), 0) << "app actions wait for each other";
  cancel = true;
  second.join();
  EXPECT_LT(Clock::now() - started, 1s);
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(*result);
  EXPECT_EQ(other_runs_.load(), 0);

  release_ = true;
  first.join();
}

TEST_F(ToolRegistryWaitTest, OnTheUiThreadAToolRunsAtOnceWithoutTheLock) {
  std::thread first([&] { (void)registry_.Execute("slow_app", pbr::Object{}); });
  ASSERT_TRUE(WaitFor([&] { return inside_slow_.load(); }));
  EXPECT_TRUE(registry_.Execute("other_app", pbr::Object{})) << "the UI thread never waits for a turn";
  EXPECT_EQ(other_runs_.load(), 1);
  release_ = true;
  first.join();
}

TEST_F(ToolRegistryWaitTest, NetworkToolsAreNotSerialized) {
  std::thread a([&] { (void)registry_.Execute("network", pbr::Object{}); });
  std::thread b([&] { (void)registry_.Execute("network", pbr::Object{}); });
  a.join();
  b.join();
  EXPECT_EQ(max_network_.load(), 2);
}

} // namespace
