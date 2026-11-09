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
       {"ring", "ring_group", "voice", "reconnect", "reconnect_recovers", "video", "failed", "answer_group",
        "group_voice", "group_video", "voice_hold", "video_hold", "group_hold", "ring_conflict", "call_back"}) {
    EXPECT_TRUE(DemoCallSceneAt(scene, 0, 1'000'000).valid) << scene;
  }
  EXPECT_FALSE(DemoCallSceneAt("nope", 0, 1'000'000).valid);
  EXPECT_EQ(DemoCallSceneAt("ring_group", 0, 1).screen.kind, CallScreen::Kind::Ringing);
  EXPECT_TRUE(DemoCallSceneAt("group_voice", 0, 1).screen.group);
  EXPECT_FALSE(DemoCallSceneAt("group_voice", 0, 1).screen.placed_here) << "joined, not placed";
  EXPECT_EQ(DemoCallSceneAt("answer_group", 4'000, 1).screen.kind, CallScreen::Kind::Answering);
  EXPECT_TRUE(DemoCallSceneAt("answer_group", 4'000, 1).screen.ring_group);
}

// V060: the hold scenes put the peer (1:1) or one member (group) on hold, and pause another member's camera.
TEST(CallDemoScenesTest, TheHoldScenesPutTheOtherSideOnHold) {
  for (const char* scene : {"voice_hold", "video_hold"}) {
    const DemoCallFrame f = DemoCallSceneAt(scene, 0, 1'000'000);
    EXPECT_EQ(f.screen.status, CallScreen::Status::Connected) << scene;
    EXPECT_FALSE(f.screen.group) << scene;
    ASSERT_EQ(f.screen.members.size(), 2u) << scene;
    EXPECT_FALSE(f.screen.members[0].is_local);
    EXPECT_TRUE(f.screen.members[0].on_hold) << scene;
    EXPECT_FALSE(f.screen.members[1].on_hold) << "we are not on hold";
  }
  EXPECT_FALSE(DemoCallSceneAt("voice_hold", 0, 1).video_stage);
  EXPECT_TRUE(DemoCallSceneAt("video_hold", 0, 1).video_stage);

  const DemoCallFrame g = DemoCallSceneAt("group_hold", 0, 1'000'000);
  EXPECT_TRUE(g.screen.group);
  EXPECT_TRUE(g.screen.video);
  EXPECT_TRUE(g.screen.members[0].on_hold);
  EXPECT_TRUE(g.screen.members[1].video_paused);
  EXPECT_FALSE(g.screen.members[1].on_hold);
}

// The scenes run through the presenter's own media decisions (CallMediaFrame), as a real call would.
const CallMediaScreen& StepScene(CallMediaFrame& frame, const char* scene, const int64_t t, const int64_t now) {
  const DemoCallFrame f = DemoCallSceneAt(scene, t, now);
  return frame.Step(f.screen, DemoCallMediaFacts(f, now), DemoCallHealth(f));
}

TEST(CallDemoScenesTest, TheHoldScenesLineNamesThePeerThroughThePresentersDecision) {
  CallMediaFrame frame;
  const CallMediaScreen& v = StepScene(frame, "voice_hold", 0, 1'000'000);
  EXPECT_TRUE(v.media.on_hold);
  EXPECT_EQ(v.line.kind, CallStatusLine::Kind::PeerOnHold);
  CallMediaFrame group;
  EXPECT_FALSE(StepScene(group, "group_hold", 0, 1'000'000).media.on_hold)
      << "a member on hold is their tile's, not the line";
}

TEST(CallDemoScenesTest, AReconnectThatRecoversSaysRestoredAndOneThatDoesNotSaysTheyMayBeOffline) {
  const int64_t start = 1'000'000;
  CallMediaFrame recovers;
  EXPECT_EQ(StepScene(recovers, "reconnect_recovers", 1'000, start + 1'000).reconnect.kind, CallReconnectKind::None);
  EXPECT_EQ(StepScene(recovers, "reconnect_recovers", 7'000, start + 7'000).reconnect.kind,
            CallReconnectKind::Reconnecting);
  EXPECT_EQ(StepScene(recovers, "reconnect_recovers", 12'500, start + 12'500).reconnect.kind,
            CallReconnectKind::Restored);

  CallMediaFrame stays_down;
  StepScene(stays_down, "reconnect", 7'000, start + 7'000);
  EXPECT_EQ(StepScene(stays_down, "reconnect", 27'000, start + 27'000).reconnect.kind, CallReconnectKind::Offline);
}

TEST(CallDemoScenesTest, TheConflictRingNamesTheCallAnsweringEnds) {
  const DemoCallFrame f = DemoCallSceneAt("ring_conflict", 0, 1);
  EXPECT_EQ(f.screen.kind, CallScreen::Kind::Ringing);
  EXPECT_TRUE(f.screen.conflict);
  EXPECT_FALSE(f.screen.ending.peer_identity.empty());
  EXPECT_NE(f.screen.ending.peer_identity, f.screen.caller_identity);
}

TEST(CallDemoScenesTest, TheCallBackSceneOffersTheEndedCallOnceTheCallTakenIsOver) {
  EXPECT_FALSE(DemoCallSceneAt("call_back", 2'000, 1'000'000).call_back);
  EXPECT_EQ(DemoCallSceneAt("call_back", 2'000, 1'000'000).screen.status, CallScreen::Status::Connected);
  const DemoCallFrame over = DemoCallSceneAt("call_back", 9'000, 1'000'000);
  ASSERT_TRUE(over.call_back);
  EXPECT_FALSE(over.call_back->peer_identity.empty());
  EXPECT_EQ(over.screen.kind, CallScreen::Kind::None);
}

TEST(CallDemoScenesTest, ASceneMayStartPartWayIn) {
  EXPECT_EQ(ParseDemoCallScene("voice@12"), (std::pair<std::string, int64_t>{"voice", 12'000}));
  EXPECT_EQ(ParseDemoCallScene("failed"), (std::pair<std::string, int64_t>{"failed", 0}));
}

} // namespace
} // namespace pbr
