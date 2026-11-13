// When a call keeps the screen on (B215): only while a video picture is up.
#include "feature/presenters/CallScreenAwake.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

CallScreenAwakeFacts Live() {
  CallScreenAwakeFacts f;
  f.has_call = true;
  f.connected = true;
  return f;
}

TEST(CallScreenAwakeTest, NoCallLetsTheScreenSleep) {
  CallScreenAwakeFacts f;
  f.camera_on = true;
  f.remote_video = true;
  EXPECT_FALSE(CallWantsScreenOn(f));
  EXPECT_STREQ(CallScreenAwakeReason(f), "ended");
}

TEST(CallScreenAwakeTest, ConnectedVoiceCallLetsTheScreenSleep) {
  CallScreenAwakeFacts f = Live();
  EXPECT_FALSE(CallWantsScreenOn(f));
  EXPECT_STREQ(CallScreenAwakeReason(f), "no_video");
}

TEST(CallScreenAwakeTest, RingingOrCallingVoiceCallLetsTheScreenSleep) {
  CallScreenAwakeFacts f;
  f.has_call = true;
  EXPECT_FALSE(CallWantsScreenOn(f));
  f.camera_on = false;
  f.video_mode = false;
  EXPECT_FALSE(CallWantsScreenOn(f));
}

TEST(CallScreenAwakeTest, VideoCallNotYetConnectedWithoutCameraLetsTheScreenSleep) {
  CallScreenAwakeFacts f;
  f.has_call = true;
  f.video_mode = true;
  EXPECT_FALSE(CallWantsScreenOn(f));
}

TEST(CallScreenAwakeTest, LocalCameraOnKeepsTheScreenOn) {
  CallScreenAwakeFacts f = Live();
  f.camera_on = true;
  EXPECT_TRUE(CallWantsScreenOn(f));
  EXPECT_STREQ(CallScreenAwakeReason(f), "video_call");
}

TEST(CallScreenAwakeTest, OutgoingVideoCallWithPreviewKeepsTheScreenOn) {
  CallScreenAwakeFacts f;
  f.has_call = true;
  f.video_mode = true;
  f.camera_on = true;
  EXPECT_TRUE(CallWantsScreenOn(f));
}

TEST(CallScreenAwakeTest, OutgoingVoiceCallWithCameraOnLetsTheScreenSleep) {
  CallScreenAwakeFacts f;
  f.has_call = true;
  f.video_mode = false;
  f.camera_on = true;
  EXPECT_FALSE(CallWantsScreenOn(f));
}

TEST(CallScreenAwakeTest, PeerVideoAloneKeepsTheScreenOn) {
  CallScreenAwakeFacts f = Live();
  f.remote_video = true;
  EXPECT_TRUE(CallWantsScreenOn(f));
}

TEST(CallScreenAwakeTest, GroupWithOneLiveVideoKeepsTheScreenOn) {
  // A group's remote_video is "at least one tile has a live stream": one is enough.
  CallScreenAwakeFacts f = Live();
  f.video_mode = true;
  f.remote_video = true;
  EXPECT_TRUE(CallWantsScreenOn(f));
}

TEST(CallScreenAwakeTest, VideoCallWithBothCamerasOffLetsTheScreenSleep) {
  CallScreenAwakeFacts f = Live();
  f.video_mode = true;
  EXPECT_FALSE(CallWantsScreenOn(f));
  EXPECT_STREQ(CallScreenAwakeReason(f), "no_video");
}

TEST(CallScreenAwakeTest, HoldLetsTheScreenSleepEvenWithVideo) {
  CallScreenAwakeFacts f = Live();
  f.camera_on = true;
  f.remote_video = true;
  f.on_hold = true;
  EXPECT_FALSE(CallWantsScreenOn(f));
  EXPECT_STREQ(CallScreenAwakeReason(f), "hold");
}

TEST(CallScreenAwakeTest, EndedCallLetsTheScreenSleep) {
  CallScreenAwakeFacts f = Live();
  f.camera_on = true;
  EXPECT_TRUE(CallWantsScreenOn(f));
  f.has_call = false;
  EXPECT_FALSE(CallWantsScreenOn(f));
}

TEST(CallScreenAwakeTest, FailedOutgoingVideoCallNeverKeepsTheScreenOn) {
  CallScreenAwakeFacts f;
  f.has_call = true;
  f.video_mode = true;
  f.camera_on = true;
  EXPECT_TRUE(CallWantsScreenOn(f)); // still trying: the preview is up
  f.failed = true;                   // "couldn't connect" / "disconnected"
  EXPECT_FALSE(CallWantsScreenOn(f));
  EXPECT_STREQ(CallScreenAwakeReason(f), "ended");
  f.connected = true;
  f.remote_video = true;
  EXPECT_FALSE(CallWantsScreenOn(f));
}

} // namespace
} // namespace pbr
