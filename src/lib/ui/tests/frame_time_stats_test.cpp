#include "lib/ui/FrameTimeStats.h"

#include <gtest/gtest.h>

namespace pbr {

TEST(FrameTimeStatsTest, EmptyWindowIsAllZero) {
  FrameTimeStats stats;
  const FrameTimeSummary s = stats.Summarize();
  EXPECT_EQ(s.frames, 0u);
  EXPECT_EQ(s.avg_fps, 0.0);
  EXPECT_EQ(s.max_ms, 0.0);
}

TEST(FrameTimeStatsTest, KnownSequence) {
  FrameTimeStats stats;
  for (int i = 0; i < 98; ++i) {
    stats.Add(10.0); // 98 frames at 10 ms
  }
  stats.Add(40.0);
  stats.Add(60.0); // one frame over 50 ms
  const FrameTimeSummary s = stats.Summarize();
  EXPECT_EQ(s.frames, 100u);
  EXPECT_NEAR(s.avg_fps, 100.0 * 1000.0 / (980.0 + 100.0), 1e-3);
  EXPECT_DOUBLE_EQ(s.max_ms, 60.0);
  EXPECT_EQ(s.over50, 1u);
  EXPECT_DOUBLE_EQ(s.p99_ms, 40.0); // nearest rank: ceil(0.99 * 100) = 99th smallest
}

TEST(FrameTimeStatsTest, ExactlyFiftyIsNotOver) {
  FrameTimeStats stats;
  stats.Add(50.0);
  stats.Add(50.5);
  EXPECT_EQ(stats.Summarize().over50, 1u);
}

TEST(FrameTimeStatsTest, SingleSampleAndReset) {
  FrameTimeStats stats;
  stats.Add(16.0);
  const FrameTimeSummary s = stats.Summarize();
  EXPECT_DOUBLE_EQ(s.p99_ms, 16.0);
  EXPECT_NEAR(s.avg_fps, 62.5, 1e-6);
  stats.Reset();
  EXPECT_EQ(stats.Count(), 0u);
}

TEST(FrameTimeStatsTest, IgnoresNonPositiveAndStopsAtCapacity) {
  FrameTimeStats stats;
  stats.Add(0.0);
  stats.Add(-1.0);
  EXPECT_EQ(stats.Count(), 0u);
  for (size_t i = 0; i < FrameTimeStats::kCapacity + 10; ++i) {
    stats.Add(8.0);
  }
  EXPECT_EQ(stats.Count(), FrameTimeStats::kCapacity);
}

} // namespace pbr
