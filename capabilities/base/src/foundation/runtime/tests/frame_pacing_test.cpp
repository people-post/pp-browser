#include "foundation/runtime/FramePacing.h"

#include <gtest/gtest.h>

#include <atomic>

namespace pbr {
namespace {

// A call's video wakes the frame loop once per frame, at most every kVideoWakeMinIntervalMs: a camera's 30 fps
// gets every frame drawn, and a peer frame that comes right after a self-view frame waits for the next wake.
TEST(FramePacingTest, VideoFramesWakeTheLoopAtMostOnceAnInterval) {
  std::atomic<int64_t> last{0};
  int64_t now = 1'000'000;
  EXPECT_TRUE(TakeVideoFrameWake(last, now)) << "the first frame wakes the loop";
  EXPECT_FALSE(TakeVideoFrameWake(last, now + 10)) << "a frame 10 ms later is drawn with the next wake";
  EXPECT_TRUE(TakeVideoFrameWake(last, now + kVideoWakeMinIntervalMs));

  // A camera at 30 fps: every frame (33 ms apart) wakes the loop.
  int wakes = 0;
  for (int i = 1; i <= 30; ++i) {
    wakes += TakeVideoFrameWake(last, now + kVideoWakeMinIntervalMs + i * 33) ? 1 : 0;
  }
  EXPECT_EQ(wakes, 30);
}

// A self-view at 30 fps and a peer at 15 fps, not in step: never more than one wake an interval.
TEST(FramePacingTest, TwoStreamsTogetherStayWithinTheCap) {
  std::atomic<int64_t> last{0};
  const int64_t start = 1'000'000;
  int wakes = 0;
  for (int64_t t = 0; t < 1000; ++t) {
    const bool self_frame = t % 33 == 0;
    const bool peer_frame = t % 67 == 11;
    for (bool frame : {self_frame, peer_frame}) {
      if (frame) {
        wakes += TakeVideoFrameWake(last, start + t) ? 1 : 0;
      }
    }
  }
  EXPECT_LE(wakes, 1000 / kVideoWakeMinIntervalMs + 1);
  EXPECT_GE(wakes, 30) << "every self-view frame still gets a wake";
}

} // namespace
} // namespace pbr
