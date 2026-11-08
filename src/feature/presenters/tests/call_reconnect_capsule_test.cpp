#include "feature/presenters/CallReconnectCapsule.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// The reconnect capsule of a running call (#171): reconnecting, restored for 2 s, offline after 20 s.

using Kind = CallReconnectKind;

TEST(CallReconnectCapsuleTest, ShowsNothingWhileTheLineIsUp) {
  CallReconnectCapsule capsule;
  const CallReconnectView& v = capsule.Step("c1", false, 1'000);
  EXPECT_EQ(v.kind, Kind::None);
  EXPECT_FALSE(v.timer_dim);
}

TEST(CallReconnectCapsuleTest, ReconnectingGreysTheTimerThenRestoredFadesAfterTwoSeconds) {
  CallReconnectCapsule capsule;
  capsule.Step("c1", false, 1'000);
  CallReconnectView v = capsule.Step("c1", true, 5'000);
  EXPECT_EQ(v.kind, Kind::Reconnecting);
  EXPECT_TRUE(v.timer_dim);
  EXPECT_STREQ(CallReconnectKey(v.kind), "call.status.reconnecting");
  v = capsule.Step("c1", false, 7'000);
  EXPECT_EQ(v.kind, Kind::Restored);
  EXPECT_FALSE(v.timer_dim);
  EXPECT_FALSE(v.fading);
  v = capsule.Step("c1", false, 8'999);
  EXPECT_EQ(v.kind, Kind::Restored);
  EXPECT_FALSE(v.fading);
  v = capsule.Step("c1", false, 9'000);
  EXPECT_EQ(v.kind, Kind::Restored);
  EXPECT_TRUE(v.fading);
  v = capsule.Step("c1", false, 9'300);
  EXPECT_EQ(v.kind, Kind::None);
}

TEST(CallReconnectCapsuleTest, AfterTwentySecondsItSaysTheyMayBeOffline) {
  CallReconnectCapsule capsule;
  capsule.Step("c1", true, 10'000);
  EXPECT_EQ(capsule.Step("c1", true, 29'999).kind, Kind::Reconnecting);
  const CallReconnectView v = capsule.Step("c1", true, 30'000);
  EXPECT_EQ(v.kind, Kind::Offline);
  EXPECT_TRUE(v.timer_dim);
  EXPECT_STREQ(CallReconnectKey(v.kind), "call.reconnect.offline");
  EXPECT_EQ(capsule.Step("c1", false, 31'000).kind, Kind::Restored) << "it can still come back";
}

TEST(CallReconnectCapsuleTest, ADropDuringRestoredStartsOverAndANewCallResets) {
  CallReconnectCapsule capsule;
  capsule.Step("c1", true, 1'000);
  capsule.Step("c1", false, 2'000);
  EXPECT_EQ(capsule.Step("c1", true, 2'500).kind, Kind::Reconnecting);
  EXPECT_EQ(capsule.Step("c2", false, 3'000).kind, Kind::None);
  EXPECT_EQ(capsule.Step("", true, 4'000).kind, Kind::None) << "no call: nothing to show";
}

} // namespace
} // namespace pbr
