#include "base/media/CallMediaAdaptation.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(CallMediaAdaptationTest, AudioAlwaysPreferredOverVideo) {
  CallAdaptationInput in;
  in.per_user_up_bps = 40'000; // enough for audio, not video
  in.camera_user_wants = true;
  const auto d = CallMediaAdaptation::Evaluate(in);
  EXPECT_TRUE(d.publish_audio);
  EXPECT_FALSE(d.publish_video_lo);
  EXPECT_FALSE(d.camera_allowed);
}

TEST(CallMediaAdaptationTest, VideoLoWhenBudgetAllows) {
  CallAdaptationInput in;
  in.per_user_up_bps = 600'000;
  in.camera_user_wants = true;
  const auto d = CallMediaAdaptation::Evaluate(in);
  EXPECT_TRUE(d.publish_audio);
  EXPECT_TRUE(d.publish_video_lo);
  EXPECT_TRUE(d.camera_allowed);
  EXPECT_FALSE(d.publish_video_hi);
  EXPECT_GT(d.target_video_lo_bps, 0);
}

TEST(CallMediaAdaptationTest, PathPressureDropsVideo) {
  CallAdaptationInput in;
  in.per_user_up_bps = 2'000'000;
  in.camera_user_wants = true;
  in.path_pressure = 0.9;
  const auto d = CallMediaAdaptation::Evaluate(in);
  EXPECT_TRUE(d.publish_audio);
  EXPECT_FALSE(d.publish_video_lo);
}

TEST(CallMediaAdaptationTest, VideoHiOnlyWhenAllowed) {
  CallAdaptationInput in;
  in.per_user_up_bps = 3'000'000;
  in.camera_user_wants = true;
  in.allow_video_hi = true;
  const auto d = CallMediaAdaptation::Evaluate(in);
  EXPECT_TRUE(d.publish_video_lo);
  EXPECT_TRUE(d.publish_video_hi);
}

TEST(CallMediaTopologyTest, GroupUsesRelay) {
  EXPECT_FALSE(CallMediaTopology::ShouldUseMediaRelay(2, false));
  // ice_failed_1to1 remains true for topology helpers; CallSessionManager only auto-SFUs N≥3.
  EXPECT_TRUE(CallMediaTopology::ShouldUseMediaRelay(2, true));
  EXPECT_TRUE(CallMediaTopology::ShouldUseMediaRelay(3, false));
  EXPECT_TRUE(CallMediaTopology::ShouldSoftMigrateToSfu(2, 3));
  EXPECT_FALSE(CallMediaTopology::ShouldSoftMigrateToSfu(3, 4));
}

} // namespace
} // namespace pbr
