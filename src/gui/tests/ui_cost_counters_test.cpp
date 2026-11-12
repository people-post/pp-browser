// perf.threads' cost counters (DEV/PLAN-call-frame-cost.md step 0): they add up, TakeAndReset hands the window's totals
// over and starts from zero, and the video-wake counter (a decoder's thread) loses nothing across threads.
#include "gui/UiFrameStats.h"

#include "foundation/platform/ui/RmlUi_Backend.h"

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

// Step 3a: who asks for the frames — the call chrome's forced frame by kind, the turns with UI tasks, every turn.
TEST(UiCostCountersTest, ForcedChromeFramesTaskTurnsAndLoopsAddUpAndClearOnTake) {
  UiCostCounters counters;
  counters.AddChromeForceFrame(CallChromeUpdate::Remount);
  counters.AddChromeForceFrame(CallChromeUpdate::DirtyOnly);
  counters.AddChromeForceFrame(CallChromeUpdate::DirtyOnly);
  counters.AddChromeForceFrame(CallChromeUpdate::PartsOnly);
  counters.AddChromeForceFrame(CallChromeUpdate::PartsOnly);
  counters.AddChromeForceFrame(CallChromeUpdate::PartsOnly);
  counters.AddChromeForceFrame(CallChromeUpdate::LevelsOnly);
  counters.AddChromeForceFrame(CallChromeUpdate::None);
  counters.AddUiTaskTurn();
  counters.AddUiTaskTurn();
  for (int i = 0; i < 5; ++i) {
    counters.AddLoop();
  }

  const UiCostTotals first = counters.TakeAndReset();
  EXPECT_EQ(first.chrome_force_frames_remount, 1);
  EXPECT_EQ(first.chrome_force_frames_dirty, 2);
  EXPECT_EQ(first.chrome_force_frames_parts, 3);
  EXPECT_EQ(first.chrome_force_frames_levels, 1) << "None is never pushed and counts nowhere";
  EXPECT_EQ(first.ui_tasks, 2);
  EXPECT_EQ(first.loops, 5);
  EXPECT_EQ(first.video_wakes, 0) << "a separate counter";

  const UiCostTotals second = counters.TakeAndReset();
  EXPECT_EQ(second.chrome_force_frames_remount, 0);
  EXPECT_EQ(second.chrome_force_frames_dirty, 0);
  EXPECT_EQ(second.chrome_force_frames_parts, 0);
  EXPECT_EQ(second.chrome_force_frames_levels, 0);
  EXPECT_EQ(second.ui_tasks, 0);
  EXPECT_EQ(second.loops, 0);
}

// Every RequestForceFrame call counts, from any thread, even before the window exists (the backend is not up here).
TEST(UiCostCountersTest, ForceFrameRequestsFromManyThreadsAreAllCountedAndClearOnTake) {
  (void)Backend::TakeForceFrameRequests();
  constexpr int kThreads = 4;
  constexpr int kPerThread = 5000;
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([]() {
      for (int n = 0; n < kPerThread; ++n) {
        Backend::RequestForceFrame();
      }
    });
  }
  long long seen = 0;
  for (int i = 0; i < 20; ++i) {
    seen += Backend::TakeForceFrameRequests();
  }
  for (std::thread& t : threads) {
    t.join();
  }
  seen += Backend::TakeForceFrameRequests();
  EXPECT_EQ(seen, static_cast<long long>(kThreads) * kPerThread);
  EXPECT_EQ(Backend::TakeForceFrameRequests(), 0);
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
