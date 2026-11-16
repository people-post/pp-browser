#include "feature/presenters/CallPlayoutStuck.h"

#include <gtest/gtest.h>

#include <vector>

namespace pbr {
namespace {

// "Their voice arrives but nothing plays" (#605): the timer decides it from three engine counters per tick.

using Sample = CallPlayoutStuck::Sample;

Sample Tick(const int64_t now_ms, const uint64_t rx, const uint64_t underruns, const int64_t depth,
            const bool counting = true) {
  Sample s;
  s.counting = counting;
  s.rx_frames = rx;
  s.underruns = underruns;
  s.jitter_depth_ms = depth;
  s.now_ms = now_ms;
  return s;
}

/** Ticks every `step_ms` over [from, to] (inclusive): per `step_ms` the counters rise by the given amounts. */
struct Feed {
  CallPlayoutStuck timer;
  uint64_t rx = 1000;
  uint64_t underruns = 0;
  int64_t now = 0;
  int edges = 0;
  int64_t first_stuck_ms = -1;

  bool Run(const int64_t seconds, const uint64_t rx_per_2s, const uint64_t underruns_per_2s, const int64_t depth,
           const bool counting = true, const int64_t step_ms = 500) {
    bool stuck = false;
    for (int64_t t = step_ms; t <= seconds * 1000; t += step_ms) {
      now += step_ms;
      rx += rx_per_2s * static_cast<uint64_t>(step_ms) / 2000;
      underruns += underruns_per_2s * static_cast<uint64_t>(step_ms) / 2000;
      const auto r = timer.Update(Tick(now, rx, underruns, depth, counting));
      stuck = r.stuck;
      edges += r.became_stuck ? 1 : 0;
      if (r.became_stuck && first_stuck_ms < 0) {
        first_stuck_ms = now;
      }
    }
    return stuck;
  }
};

constexpr uint64_t kHealthyRx = 50;
constexpr uint64_t kFaultRx = 50;
constexpr uint64_t kFaultUnderruns = 100;

TEST(CallPlayoutStuckTest, ANormalCallIsNeverStuck) {
  Feed f;
  f.timer.Update(Tick(0, 1000, 0, 80));
  for (int i = 0; i < 30; ++i) {
    EXPECT_FALSE(f.Run(2, kHealthyRx, i % 2, 80));
  }
  EXPECT_EQ(f.edges, 0);
}

TEST(CallPlayoutStuckTest, TheFaultShapeTurnsTrueAtSixSecondsAndStays) {
  Feed f;
  f.timer.Update(Tick(0, 1000, 0, 0));
  EXPECT_FALSE(f.Run(5, kFaultRx, kFaultUnderruns, 0));
  EXPECT_EQ(f.edges, 0);
  EXPECT_TRUE(f.Run(1, kFaultRx, kFaultUnderruns, 0));
  EXPECT_EQ(f.first_stuck_ms, 6'000);
  EXPECT_TRUE(f.Run(30, kFaultRx, kFaultUnderruns, 0));
  EXPECT_EQ(f.edges, 1) << "one edge per spell";
}

TEST(CallPlayoutStuckTest, ARecoveryAfterFourSecondsNeverTurnsTrueAndTheNextFaultCountsSixAgain) {
  Feed f;
  f.timer.Update(Tick(0, 1000, 0, 0));
  EXPECT_FALSE(f.Run(4, kFaultRx, kFaultUnderruns, 0));
  EXPECT_FALSE(f.Run(4, kHealthyRx, 0, 80));
  EXPECT_FALSE(f.Run(4, kFaultRx, kFaultUnderruns, 0)) << "four seconds of the fault again: not yet";
  EXPECT_EQ(f.edges, 0);
  EXPECT_TRUE(f.Run(4, kFaultRx, kFaultUnderruns, 0));
  EXPECT_EQ(f.edges, 1);
}

TEST(CallPlayoutStuckTest, ThePeerMutedWithAHealthyBufferIsNotStuck) {
  Feed f;
  f.timer.Update(Tick(0, 1000, 0, 40));
  EXPECT_FALSE(f.Run(60, kHealthyRx, 0, 40));
}

TEST(CallPlayoutStuckTest, ThePeerSendingNothingIsAnotherFaultNotThisOne) {
  Feed f;
  f.timer.Update(Tick(0, 1000, 0, 0));
  EXPECT_FALSE(f.Run(60, 0, kFaultUnderruns, 0));
}

TEST(CallPlayoutStuckTest, APoorNetworkWhoseDepthIsSometimesAboveZeroIsNeverStuck) {
  CallPlayoutStuck timer;
  uint64_t rx = 1000;
  uint64_t underruns = 0;
  bool ever = false;
  timer.Update(Tick(0, rx, underruns, 0));
  // Underruns every window as in the fault, but the buffer holds a frame at some tick inside every window.
  for (int64_t t = 500; t <= 60'000; t += 500) {
    rx += 12;
    underruns += 25;
    const int64_t depth = (t % 2'000 == 1'000) ? 20 : 0;
    ever |= timer.Update(Tick(t, rx, underruns, depth)).stuck;
  }
  EXPECT_FALSE(ever);
}

TEST(CallPlayoutStuckTest, ATickGapOfEightSecondsIsNoFalsePositive) {
  CallPlayoutStuck timer;
  // The fault shape stretched over one long gap would look like four windows' worth: it must not count.
  timer.Update(Tick(0, 1000, 0, 0));
  EXPECT_FALSE(timer.Update(Tick(8'000, 1200, 400, 0)).stuck);
  // Nor does it carry a half-counted window: three more windows are needed from the new baseline.
  Feed f;
  f.timer = timer;
  f.now = 8'000;
  f.rx = 1200;
  f.underruns = 400;
  EXPECT_FALSE(f.Run(4, kFaultRx, kFaultUnderruns, 0));
  EXPECT_TRUE(f.Run(2, kFaultRx, kFaultUnderruns, 0));
}

TEST(CallPlayoutStuckTest, TheThresholdsScaleWithTheRealElapsedTime) {
  CallPlayoutStuck timer;
  // Ticks 4.5 s apart (under the gap limit): the same rates over a longer window still count as stuck, and a window
  // that has only the 2 s numbers over 4.5 s does not.
  timer.Update(Tick(0, 1000, 0, 0));
  EXPECT_FALSE(timer.Update(Tick(4'500, 1000 + 40, 40, 0)).stuck) << "40 frames in 4.5 s is below 30 per 2 s";
  CallPlayoutStuck steady;
  steady.Update(Tick(0, 1000, 0, 0));
  bool stuck = false;
  for (int i = 1; i <= 3; ++i) {
    stuck = steady.Update(Tick(i * 4'500, 1000 + i * 112, i * 225, 0)).stuck;
  }
  EXPECT_TRUE(stuck) << "112 frames and 225 underruns per 4.5 s are the fault's rates";
}

TEST(CallPlayoutStuckTest, ThePeerOnHoldWithTheFaultShapeIsNotCountedAndAfterItSixSecondsTurnsItTrue) {
  Feed f;
  f.timer.Update(Tick(0, 1000, 0, 0));
  EXPECT_FALSE(f.Run(30, kFaultRx, kFaultUnderruns, 0, /*counting=*/false));
  EXPECT_EQ(f.edges, 0);
  // The first tick after the hold (0.5 s) is the fresh baseline; its three windows close at 2.5, 4.5 and 6.5 s.
  EXPECT_FALSE(f.Run(6, kFaultRx, kFaultUnderruns, 0));
  EXPECT_TRUE(f.Run(1, kFaultRx, kFaultUnderruns, 0)) << "six seconds after the fresh baseline";
  EXPECT_EQ(f.edges, 1);
}

TEST(CallPlayoutStuckTest, AHoldEndingBetweenTicksDoesNotCountTheWindowAcrossIt) {
  CallPlayoutStuck timer;
  // A window opened before the hold would be stuck by its numbers (the hold sits in the middle of it).
  timer.Update(Tick(0, 1000, 0, 0));
  timer.Update(Tick(1'000, 1025, 50, 0, /*counting=*/false)); // hold begins: baseline dropped
  // The hold ends at about 1.9 s; the first counted tick is the baseline of a fresh window.
  EXPECT_FALSE(timer.Update(Tick(2'100, 1050, 100, 0)).stuck);
  EXPECT_FALSE(timer.Update(Tick(3'100, 1075, 150, 0)).stuck) << "only 1 s after the new baseline: no window yet";
  EXPECT_FALSE(timer.Update(Tick(4'100, 1100, 200, 0)).stuck) << "window 1 closes at 4.1 s";
  EXPECT_FALSE(timer.Update(Tick(6'100, 1150, 300, 0)).stuck) << "window 2";
  EXPECT_TRUE(timer.Update(Tick(8'100, 1200, 400, 0)).stuck) << "window 3: six seconds after the fresh baseline";
}

TEST(CallPlayoutStuckTest, LocalHoldReconnectingAndNotConnectedCountNothingAndResetTheCount) {
  Feed f;
  f.timer.Update(Tick(0, 1000, 0, 0));
  EXPECT_FALSE(f.Run(4, kFaultRx, kFaultUnderruns, 0)); // two windows counted
  EXPECT_FALSE(f.Run(1, kFaultRx, kFaultUnderruns, 0, /*counting=*/false)) << "held / reconnecting / not connected";
  EXPECT_FALSE(f.Run(6, kFaultRx, kFaultUnderruns, 0)) << "the count started over (fresh baseline at +0.5 s)";
  EXPECT_EQ(f.edges, 0);
  EXPECT_TRUE(f.Run(1, kFaultRx, kFaultUnderruns, 0));
}

TEST(CallPlayoutStuckTest, ACounterGoingBackwardsResetsInsteadOfWrapping) {
  Feed f;
  f.timer.Update(Tick(0, 1000, 0, 0));
  EXPECT_FALSE(f.Run(4, kFaultRx, kFaultUnderruns, 0));
  // The engine restarted: counters begin again near zero. An unsigned difference would be a huge "stuck" window.
  EXPECT_FALSE(f.timer.Update(Tick(f.now + 500, 10, 3, 0)).stuck);
  f.rx = 10;
  f.underruns = 3;
  f.now += 500;
  EXPECT_FALSE(f.Run(4, kFaultRx, kFaultUnderruns, 0)) << "the two earlier windows are gone";
  EXPECT_EQ(f.edges, 0);
  EXPECT_TRUE(f.Run(2, kFaultRx, kFaultUnderruns, 0));
}

TEST(CallPlayoutStuckTest, TheEdgeIsReportedOncePerSpell) {
  Feed f;
  f.timer.Update(Tick(0, 1000, 0, 0));
  EXPECT_TRUE(f.Run(20, kFaultRx, kFaultUnderruns, 0));
  EXPECT_EQ(f.edges, 1) << "stuck for many ticks, reported once";
  EXPECT_FALSE(f.Run(4, kHealthyRx, 0, 80)) << "recovered";
  EXPECT_TRUE(f.Run(10, kFaultRx, kFaultUnderruns, 0));
  EXPECT_EQ(f.edges, 2) << "stuck, recovered, stuck again: exactly two";
}

// The real numbers: the vivo's `media_health` lines of the 2026-11-13 call in which the other phone's Siri took its
// audio (logs/archive/vivo-2026-11-13-siri-round4/logcat.txt, 15:57:53 - 15:58:20): rx_frames / underrun / jb_depth_ms
// at each line's own time. The peer was on hold from 15:57:56.233 (the status line read "stepped away") until
// 15:58:03.121 (the timer was back); the sound never came back after that.
struct LogRow {
  int s_of_minute; // seconds within the minute, with the minute below
  int ms;
  int minute; // 57 or 58 (hour 15)
  uint64_t rx;
  uint64_t underruns;
  int64_t depth;
};

constexpr int64_t AtMs(const int minute, const int second, const int ms) {
  return ((15 * 60 + minute) * 60 + second) * 1000 + ms;
}

constexpr LogRow kRows[] = {
    {53, 183, 57, 1712, 0, 40},   {55, 184, 57, 1758, 12, 120}, {57, 185, 57, 1789, 105, 0},
    {59, 188, 57, 1833, 205, 0},  {1, 194, 58, 1877, 306, 0},   {3, 696, 58, 1911, 431, 0},
    {5, 696, 58, 1961, 531, 0},   {7, 699, 58, 2010, 631, 0},   {9, 703, 58, 2060, 732, 0},
    {12, 199, 58, 2121, 856, 0},  {14, 200, 58, 2171, 958, 0},  {16, 204, 58, 2221, 1058, 0},
    {18, 204, 58, 2271, 1159, 0}, {20, 207, 58, 2321, 1258, 0},
};

TEST(CallPlayoutStuckTest, TheRealSiriCallTurnsTrueSixSecondsAfterTheHoldEndedNotBefore) {
  const int64_t hold_from = AtMs(57, 56, 233);
  const int64_t hold_to = AtMs(58, 3, 121);
  CallPlayoutStuck timer;
  int64_t first_true = -1;
  for (const LogRow& row : kRows) {
    const int64_t at = AtMs(row.minute, row.s_of_minute, row.ms);
    const bool on_hold = at >= hold_from && at < hold_to;
    const auto r = timer.Update(Tick(at, row.rx, row.underruns, row.depth, /*counting=*/!on_hold));
    if (r.stuck && first_true < 0) {
      first_true = at;
    }
  }
  EXPECT_EQ(first_true, AtMs(58, 9, 703)) << "15:58:09.703: three full windows after the baseline at 15:58:03.696";
  EXPECT_FALSE(AtMs(58, 7, 699) == first_true);
}

TEST(CallPlayoutStuckTest, TheRealCallNeverTurnsTrueDuringTheHold) {
  CallPlayoutStuck timer;
  for (const LogRow& row : kRows) {
    const int64_t at = AtMs(row.minute, row.s_of_minute, row.ms);
    if (at >= AtMs(58, 3, 121)) {
      break;
    }
    // Held from 15:57:56.233: counting is off for those ticks, as it is in the app.
    const bool on_hold = at >= AtMs(57, 56, 233);
    EXPECT_FALSE(timer.Update(Tick(at, row.rx, row.underruns, row.depth, !on_hold)).stuck);
  }
}

} // namespace
} // namespace pbr
