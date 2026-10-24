#include "lib/calls/CallScreen.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Status = CallScreen::Status;
using MediaState = CallMediaSeat::MediaState;

InCallStatus Decide(InCallSignals s) {
  return DecideInCallStatus(s);
}

TEST(CallScreenTest, NothingStartedYetReadsCalling) {
  InCallSignals s;
  EXPECT_EQ(Decide(s).status, Status::Calling);
  EXPECT_FALSE(Decide(s).media_connected);
  EXPECT_TRUE(Decide(s).show_invite);
}

TEST(CallScreenTest, LiveMediaIsConnectedWithTheTimerAndNoRetry) {
  InCallSignals s;
  s.media_chrome_live = true;
  s.p2p_connect_failed = false;
  const InCallStatus out = Decide(s);
  EXPECT_EQ(out.status, Status::Connected);
  EXPECT_TRUE(out.media_connected);
  EXPECT_TRUE(out.show_elapsed);
  EXPECT_FALSE(out.show_retry);
}

TEST(CallScreenTest, AFailedMeshPathCouldNotConnectWithItsHintAndRetry) {
  InCallSignals s;
  s.p2p_connect_failed = true;
  InCallStatus out = Decide(s);
  EXPECT_EQ(out.status, Status::CouldntConnect);
  EXPECT_EQ(out.hint, CallScreen::Hint::P2pConnect);
  EXPECT_TRUE(out.show_retry);
  EXPECT_FALSE(out.show_invite);

  s.group_context = true;
  EXPECT_EQ(Decide(s).hint, CallScreen::Hint::GroupMedia);
  s.sfu_mode = true;  // the hop owns recovery: no Retry
  EXPECT_FALSE(Decide(s).show_retry);
}

TEST(CallScreenTest, ActivityShowsUntilMediaIsLive) {
  InCallSignals s;
  s.activity = "Switching media path";
  s.media_status = CallMediaStatus::Migrating;
  InCallStatus out = Decide(s);
  EXPECT_EQ(out.status, Status::Activity);
  EXPECT_FALSE(out.activity_spent);

  s.media_chrome_live = true;  // live: the activity is spent
  out = Decide(s);
  EXPECT_TRUE(out.activity_spent);
  EXPECT_EQ(out.status, Status::Connected);

  s.recovery = CallRecovery::Reattaching;  // still recovering the group path: the activity stays
  out = Decide(s);
  EXPECT_FALSE(out.activity_spent);
  EXPECT_EQ(out.status, Status::Activity);
}

TEST(CallScreenTest, AFailureAfterBeingConnectedReadsDisconnected) {
  InCallSignals s;
  s.media_status = CallMediaStatus::Failed;
  EXPECT_EQ(Decide(s).status, Status::CouldntConnect) << "never connected: the user may retry";
  s.was_connected = true;
  EXPECT_EQ(Decide(s).status, Status::Disconnected);
  InCallSignals mesh;  // the mesh path reports the failure after the reconnect window: still "disconnected"
  mesh.p2p_connect_failed = true;
  mesh.was_connected = true;
  const InCallStatus out = Decide(mesh);
  EXPECT_EQ(out.status, Status::Disconnected);
  EXPECT_FALSE(out.show_retry);
}

TEST(CallScreenTest, ReconnectingKeepsTheTimerAndOutranksConnected) {
  InCallSignals s;
  s.media_status = CallMediaStatus::Reconnecting;
  s.media_chrome_live = true;
  const InCallStatus out = Decide(s);
  EXPECT_EQ(out.status, Status::Reconnecting);
  EXPECT_TRUE(out.show_elapsed);

  InCallSignals degraded;
  degraded.media_status = CallMediaStatus::DegradedTxOnly;
  degraded.media_chrome_live = true;
  EXPECT_EQ(Decide(degraded).status, Status::SendingOnly);
}

TEST(CallScreenTest, TheHopPathSteps) {
  InCallSignals s;
  s.media_active = true;
  s.media_status = CallMediaStatus::Migrating;
  EXPECT_EQ(Decide(s).status, Status::SettingUpGroup);
  s.media_status = CallMediaStatus::HopWaiting;
  EXPECT_EQ(Decide(s).status, Status::WaitingForMediaPath);
  s.media_status = CallMediaStatus::HopAttaching;
  EXPECT_EQ(Decide(s).status, Status::Connecting);
  s.recovery = CallRecovery::Migrating;  // a latch only: HopAttaching still owns the line
  EXPECT_EQ(Decide(s).status, Status::Connecting);
  s.media_status = CallMediaStatus::DirectConnecting;  // the latch shows once nothing else does
  EXPECT_EQ(Decide(s).status, Status::SettingUpGroup);
  s.recovery = CallRecovery::WaitingForHop;
  EXPECT_EQ(Decide(s).status, Status::WaitingForMediaPath);
}

TEST(CallScreenTest, AFailedPathCouldNotConnectWithoutAHint) {
  InCallSignals s;
  s.media_status = CallMediaStatus::Failed;
  InCallStatus out = Decide(s);
  EXPECT_EQ(out.status, Status::CouldntConnect);
  EXPECT_EQ(out.hint, CallScreen::Hint::None);
  EXPECT_TRUE(out.show_invite);
}

TEST(CallScreenTest, TheEngineStateWhenNothingElseSays) {
  InCallSignals s;
  s.media_active = true;
  s.media_status = CallMediaStatus::DirectLive;  // not connecting, not live (chrome) yet
  s.connection_state = "connecting";
  EXPECT_EQ(Decide(s).status, Status::Connecting);
  s.connection_state = "disconnected";
  EXPECT_EQ(Decide(s).status, Status::Reconnecting);
  s.connection_state = "failed";
  EXPECT_EQ(Decide(s).status, Status::CouldntConnect);
  s.connection_state = "closed";
  EXPECT_EQ(Decide(s).status, Status::Connecting);
  s.connection_state = "checking";
  EXPECT_EQ(Decide(s).status, Status::Raw);
  s.recovery = CallRecovery::Reattaching;  // the group path is being recovered: before the engine state
  EXPECT_EQ(Decide(s).status, Status::Reconnecting);
  s.recovery = CallRecovery::HopLost;  // attempts made, none in flight: still nothing flows
  EXPECT_EQ(Decide(s).status, Status::Reconnecting);
  EXPECT_FALSE(RecoveryInFlight(CallRecovery::HopLost)) << "path decisions see no recovery in flight";
  EXPECT_TRUE(RecoveryInFlight(CallRecovery::Reattaching));

  InCallSignals connecting;  // the call is connecting before the engine runs
  connecting.media_active = false;
  connecting.media_status = CallMediaStatus::DirectConnecting;
  EXPECT_EQ(Decide(connecting).status, Status::Connecting);

  InCallSignals reattaching;  // a guest's hop reattach: the call is not live, whatever the seat says
  reattaching.media_active = true;
  reattaching.media_status = CallMediaStatus::HopAttaching;
  reattaching.recovery = CallRecovery::Reattaching;
  reattaching.was_connected = true;
  EXPECT_FALSE(Decide(reattaching).media_connected);
}

} // namespace
} // namespace pbr
