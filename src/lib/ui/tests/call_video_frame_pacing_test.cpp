#include "lib/ui/ShellTypes.h"
#include "foundation/runtime/FramePacing.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

CallInProgressState ActiveCall() {
  CallInProgressState call;
  call.active = true;
  return call;
}

// The frame loop runs at the camera's rate while any video tile of the call is drawn, and only then.
TEST(CallVideoFramePacingTest, AVideoTileOnScreenRunsFramesAtTheCameraRate) {
  EXPECT_FALSE(CallVideoOnScreen(ActiveCall())) << "a voice call: the live-chrome cadence is enough";

  CallInProgressState self_view = ActiveCall();
  self_view.local_preview = true;
  EXPECT_TRUE(CallVideoOnScreen(self_view));

  CallInProgressState peer = ActiveCall();
  peer.remote_video = true;
  EXPECT_TRUE(CallVideoOnScreen(peer));

  CallInProgressState roster = ActiveCall();
  roster.roster.resize(3);
  EXPECT_FALSE(CallVideoOnScreen(roster));
  roster.roster[2].has_remote_video = true;
  EXPECT_TRUE(CallVideoOnScreen(roster));

  CallInProgressState ended = self_view;
  ended.active = false;
  EXPECT_FALSE(CallVideoOnScreen(ended));

  EXPECT_LE(kVideoFrameIntervalSec, 1.0 / 30.0 + 1e-9) << "a frame for every camera frame";
  EXPECT_LT(kVideoFrameIntervalSec, kLiveChromeFrameIntervalSec);
}

} // namespace
} // namespace pbr
