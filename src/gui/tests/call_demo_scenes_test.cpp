#include "gui/CallDemoScenes.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// The demo app's scripted calls (kUiDemoBuild only reads them) walk the real call states in order.

TEST(CallDemoScenesTest, TheVoiceSceneCallsThenConnectsRelayedThenFindsACloserWay) {
  const int64_t now = 1'000'000;
  EXPECT_EQ(DemoCallSceneAt("voice", 1'000, now).screen.status, CallScreen::Status::Calling);
  EXPECT_EQ(DemoCallSceneAt("voice", 5'000, now).screen.status, CallScreen::Status::Connecting);
  const DemoCallFrame live = DemoCallSceneAt("voice", 10'000, now);
  EXPECT_EQ(live.screen.status, CallScreen::Status::Connected);
  EXPECT_TRUE(live.screen.media_connected);
  EXPECT_EQ(live.path_kind, "circuit");
  EXPECT_EQ(live.screen.connected_at_ms, now - 3'000);
  EXPECT_EQ(DemoCallSceneAt("voice", 26'000, now).path_kind, "punched");
  EXPECT_EQ(DemoCallSceneAt("voice", 36'000, now).quality, CallPathQuality::Fair);
}

TEST(CallDemoScenesTest, EverySceneIsKnownAndAnUnknownOneIsNot) {
  for (const char* scene :
       {"ring", "ring_group", "voice", "reconnect", "video", "failed", "group_voice", "group_video"}) {
    EXPECT_TRUE(DemoCallSceneAt(scene, 0, 1'000'000).valid) << scene;
  }
  EXPECT_FALSE(DemoCallSceneAt("nope", 0, 1'000'000).valid);
  EXPECT_EQ(DemoCallSceneAt("ring_group", 0, 1).screen.kind, CallScreen::Kind::Ringing);
  EXPECT_TRUE(DemoCallSceneAt("group_voice", 0, 1).screen.group);
}

TEST(CallDemoScenesTest, ASceneMayStartPartWayIn) {
  EXPECT_EQ(ParseDemoCallScene("voice@12"), (std::pair<std::string, int64_t>{"voice", 12'000}));
  EXPECT_EQ(ParseDemoCallScene("failed"), (std::pair<std::string, int64_t>{"failed", 0}));
}

} // namespace
} // namespace pbr
