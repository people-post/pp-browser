// perf.threads' cost counters (DEV/PLAN-call-frame-cost.md step 0): they add up, TakeAndReset hands the window's totals
// over and starts from zero, and the video-wake counter (a decoder's thread) loses nothing across threads.
#include "gui/UiFrameStats.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

namespace pbr {
namespace {

TEST(UiCostCountersTest, AddsUpAndClearsOnTake) {
  UiCostCounters counters;
  counters.AddCallTickUs(1500);
  counters.AddCallTickUs(500);
  counters.AddChatUpdateUs(30);
  counters.AddShellUpdateUs(40);
  counters.AddCtxUpdateUs(5000);
  counters.AddRosterUs(700);
  counters.AddTilesUs(900);
  counters.AddRosterBuild();
  counters.AddRosterBuild();
  counters.AddIconLookup();
  counters.AddFrameCopy(false);
  counters.AddFrameCopy(true);
  counters.AddFrameCopy(true);
  counters.AddVideoWake();

  const UiCostTotals first = counters.TakeAndReset();
  EXPECT_EQ(first.call_tick_us, 2000);
  EXPECT_EQ(first.chat_update_us, 30);
  EXPECT_EQ(first.shell_update_us, 40);
  EXPECT_EQ(first.ctx_update_us, 5000);
  EXPECT_EQ(first.roster_us, 700);
  EXPECT_EQ(first.tiles_us, 900);
  EXPECT_EQ(first.roster_builds, 2);
  EXPECT_EQ(first.icon_lookups, 1);
  EXPECT_EQ(first.frame_copies, 3);
  EXPECT_EQ(first.frame_copies_stale, 2) << "a stale copy counts as a copy too";
  EXPECT_EQ(first.video_wakes, 1);

  const UiCostTotals second = counters.TakeAndReset();
  EXPECT_EQ(second.call_tick_us, 0);
  EXPECT_EQ(second.ctx_update_us, 0);
  EXPECT_EQ(second.roster_builds, 0);
  EXPECT_EQ(second.frame_copies, 0);
  EXPECT_EQ(second.frame_copies_stale, 0);
  EXPECT_EQ(second.video_wakes, 0);

  counters.AddCallTickUs(7);
  EXPECT_EQ(counters.TakeAndReset().call_tick_us, 7) << "a new window starts from zero";
}

TEST(UiCostCountersTest, VideoWakesFromManyThreadsAreAllCounted) {
  UiCostCounters counters;
  constexpr int kThreads = 8;
  constexpr int kPerThread = 20000;
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&counters]() {
      for (int n = 0; n < kPerThread; ++n) {
        counters.AddVideoWake();
      }
    });
  }
  // The UI thread takes while they run: nothing is lost between the takes.
  int64_t seen = 0;
  for (int i = 0; i < 50; ++i) {
    seen += counters.TakeAndReset().video_wakes;
  }
  for (std::thread& t : threads) {
    t.join();
  }
  seen += counters.TakeAndReset().video_wakes;
  EXPECT_EQ(seen, static_cast<int64_t>(kThreads) * kPerThread);
}

TEST(UiCostCountersTest, TimerAddsTheElapsedMicrosecondsToItsCounter) {
  UiCostCounters& counters = UiCostCounters::Instance();
  counters.TakeAndReset();
  {
    UiCostTimer timer(&UiCostCounters::AddRosterUs);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
  }
  const UiCostTotals totals = counters.TakeAndReset();
  EXPECT_GE(totals.roster_us, 3000);
  EXPECT_EQ(totals.tiles_us, 0);
}

} // namespace
} // namespace pbr
