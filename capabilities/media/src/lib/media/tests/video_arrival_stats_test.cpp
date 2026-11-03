#include "lib/media/VideoArrivalStats.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(VideoArrivalStatsTest, EvenArrivalsHaveNoStallsAndNoBunching) {
  VideoArrivalStats stats;
  for (uint32_t seq = 1; seq <= 100; ++seq) {
    stats.Note(seq, 1000 + 50 * static_cast<int64_t>(seq));
  }
  const VideoArrivalSummary s = stats.Summary();
  EXPECT_EQ(s.frames, 100u);
  EXPECT_EQ(s.lost, 0u);
  EXPECT_EQ(s.gap_p50_ms, 50);
  EXPECT_EQ(s.gap_p95_ms, 50);
  EXPECT_EQ(s.gap_max_ms, 50);
  EXPECT_EQ(s.stalls, 0u);
  EXPECT_EQ(s.bunched, 0u);
}

// What a reliable, ordered lane does to video on a lost packet: nothing for a retransmit's time, then the
// frames held behind it all at once.
TEST(VideoArrivalStatsTest, AStallThenABurstShowsAsBoth) {
  VideoArrivalStats stats;
  int64_t now = 0;
  uint32_t seq = 0;
  for (int round = 0; round < 10; ++round) {
    now += 300; // held back
    for (int i = 0; i < 6; ++i) {
      stats.Note(++seq, now + i); // let go together
    }
  }
  const VideoArrivalSummary s = stats.Summary();
  EXPECT_EQ(s.frames, 60u);
  EXPECT_EQ(s.lost, 0u) << "nothing lost: only late";
  EXPECT_EQ(s.stalls, 9u) << "a stall before every burst but the first";
  EXPECT_EQ(s.bunched, 50u);
  EXPECT_LT(s.gap_p50_ms, 10);
  EXPECT_GE(s.gap_p95_ms, 290);
  EXPECT_GE(s.gap_max_ms, 295);
}

TEST(VideoArrivalStatsTest, GapsInTheSendersNumberingAreLost) {
  VideoArrivalStats stats;
  stats.Note(1, 0);
  stats.Note(2, 50);
  stats.Note(5, 100); // 3 and 4 never came
  stats.Note(4, 120); // out of order: counted once, and the skip stays
  stats.Note(6, 150);
  EXPECT_EQ(stats.Summary().lost, 2u);
  EXPECT_EQ(stats.Summary().frames, 5u);
}

} // namespace
} // namespace pbr
