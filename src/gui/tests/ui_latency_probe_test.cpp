#include "gui/UiLatencyProbe.h"

#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace pbr {
namespace {

/** A fake UI thread: tasks queue up until the test runs them. */
struct FakeUi {
  std::mutex mu;
  std::deque<std::function<void()>> tasks;
  void Post(std::function<void()> task) {
    std::lock_guard lock(mu);
    tasks.push_back(std::move(task));
  }
  size_t Pending() {
    std::lock_guard lock(mu);
    return tasks.size();
  }
  void RunAll() {
    std::deque<std::function<void()>> run;
    {
      std::lock_guard lock(mu);
      run.swap(tasks);
    }
    for (auto& task : run) {
      task();
    }
  }
};

bool WaitFor(const std::function<bool()>& done) {
  for (int i = 0; i < 300 && !done(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return done();
}

// A blocked UI thread shows up as the ping's delivery delay, and only one ping is ever in flight.
TEST(UiLatencyProbeTest, ReportsHowLongTheUiThreadTookToRunAPing) {
  FakeUi ui;
  std::vector<int64_t> latencies;
  UiLatencyProbe probe;
  probe.Start([&](std::function<void()> task) { ui.Post(std::move(task)); },
              [&](int64_t ms) { latencies.push_back(ms); });
  ASSERT_TRUE(WaitFor([&] { return ui.Pending() == 1; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(UiLatencyProbe::kIntervalMs + 200));
  EXPECT_EQ(ui.Pending(), 1u) << "no second ping while the first is stuck";
  ui.RunAll();
  ASSERT_EQ(latencies.size(), 1u);
  EXPECT_GE(latencies[0], 600);
  probe.Stop();
}

TEST(UiLatencyProbeTest, AfterStopAQueuedPingNoLongerReports) {
  FakeUi ui;
  int reports = 0;
  UiLatencyProbe probe;
  probe.Start([&](std::function<void()> task) { ui.Post(std::move(task)); }, [&](int64_t) { ++reports; });
  ASSERT_TRUE(WaitFor([&] { return ui.Pending() == 1; }));
  probe.Stop();
  EXPECT_FALSE(probe.Running());
  ui.RunAll();
  EXPECT_EQ(reports, 0);
}

} // namespace
} // namespace pbr
