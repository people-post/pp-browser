#include "lib/media/DecodeStallPolicy.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Policy = DecodeStallPolicy;
using Budget = DecoderResetBudget;

constexpr int64_t kT0 = 1'000'000;

/** Queues `frames` frames spread evenly over `span_ms` after a configure at kT0. */
void QueueFrames(Policy& p, int frames, int64_t span_ms) {
  for (int i = 1; i <= frames; ++i) {
    p.NoteFrameQueued(kT0 + span_ms * i / frames);
  }
}

// B125: a starting MediaCodec takes frames for seconds before its first output — not a stall.
TEST(DecodeStallPolicyTest, FiftyNineFramesOverSixSecondsIsNotStalled) {
  Policy p;
  p.Restart(kT0);
  QueueFrames(p, 59, 6000);
  EXPECT_FALSE(p.Stalled(kT0 + 6000));
}

TEST(DecodeStallPolicyTest, SixtyFramesWithinFourSecondsIsNotStalled) {
  Policy p;
  p.Restart(kT0);
  QueueFrames(p, 60, 4000);
  EXPECT_FALSE(p.Stalled(kT0 + 4000));
}

TEST(DecodeStallPolicyTest, SixtyFramesAndFiveSecondsWithoutOutputIsStalled) {
  Policy p;
  p.Restart(kT0);
  QueueFrames(p, 60, 5000);
  EXPECT_TRUE(p.Stalled(kT0 + 5000));
  EXPECT_EQ(p.FramesSinceOutput(), 60);
  EXPECT_EQ(p.MsSinceOutput(kT0 + 5000), 5000);
  p.NoteOutput(kT0 + 5001);
  EXPECT_FALSE(p.Stalled(kT0 + 5001)) << "an output clears it";
}

// Frames queued while a requested keyframe is on its way are not counted, and the decoder is not
// called stalled during that wait.
TEST(DecodeStallPolicyTest, FramesWhileWaitingForAKeyframeAreNotCounted) {
  Policy p;
  p.Restart(kT0);
  p.NoteKeyframeRequested(kT0);
  QueueFrames(p, 100, Policy::kKeyframeWaitMs - 1);
  EXPECT_EQ(p.FramesSinceOutput(), 0);
  EXPECT_FALSE(p.Stalled(kT0 + Policy::kKeyframeWaitMs - 1));

  Policy q;
  q.Restart(kT0);
  QueueFrames(q, 60, 5000);
  q.NoteKeyframeRequested(kT0 + 5000);
  EXPECT_FALSE(q.Stalled(kT0 + 5000 + Policy::kKeyframeWaitMs - 1)) << "still waiting for the IDR";
  EXPECT_TRUE(q.Stalled(kT0 + 5000 + Policy::kKeyframeWaitMs)) << "the wait is over, still nothing";
}

TEST(DecodeStallPolicyTest, RestartClearsTheCount) {
  Policy p;
  p.Restart(kT0);
  QueueFrames(p, 60, 5000);
  ASSERT_TRUE(p.Stalled(kT0 + 5000));
  p.Restart(kT0 + 5000);
  EXPECT_EQ(p.FramesSinceOutput(), 0);
  EXPECT_FALSE(p.Stalled(kT0 + 5000));
  p.NoteFrameQueued(kT0 + 11'000);
  EXPECT_FALSE(p.Stalled(kT0 + 11'000)) << "time alone is not a stall";
}

// Two stalls in a row rebuild; at most twice per call, ten seconds apart; a frame breaks the row.
TEST(DecodeStallPolicyTest, ResetsNeedTwoStallsInARowAtMostTwicePerCallTenSecondsApart) {
  Budget b;
  EXPECT_FALSE(b.NoteStall(kT0)) << "one stall only asks for a keyframe";
  b.NoteFrame();
  EXPECT_FALSE(b.NoteStall(kT0 + 100)) << "a frame came out in between";
  EXPECT_TRUE(b.NoteStall(kT0 + 200));
  EXPECT_EQ(b.Resets(), 1);

  EXPECT_FALSE(b.NoteStall(kT0 + 300));
  EXPECT_FALSE(b.NoteStall(kT0 + 400)) << "under ten seconds since the last rebuild";
  EXPECT_TRUE(b.NoteStall(kT0 + 200 + Budget::kMinResetGapMs));
  EXPECT_EQ(b.Resets(), 2);
  EXPECT_TRUE(b.Exhausted());

  EXPECT_FALSE(b.NoteStall(kT0 + 60'000));
  EXPECT_FALSE(b.NoteStall(kT0 + 70'000)) << "no third rebuild in a call";
  EXPECT_EQ(b.Resets(), 2);
}

} // namespace
} // namespace pbr
