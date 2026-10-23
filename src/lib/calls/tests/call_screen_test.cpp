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

  InCallSignals seat;  // the seat's Live is a fallback while the status catches up (V037)
  seat.seat_media = MediaState::Live;
  EXPECT_EQ(Decide(seat).status, Status::Connected);
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

  s.awaiting_sfu_recovery = true;  // still recovering the group path: the activity stays
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
  InCallSignals seat;
  seat.seat_media = MediaState::Failed;
  seat.was_connected = true;
  EXPECT_EQ(Decide(seat).status, Status::Disconnected);
  InCallSignals mesh;  // the mesh path reports the failure after the reconnect window: still "disconnected"
  mesh.p2p_connect_failed = true;
  mesh.was_connected = true;
  const InCallStatus out = Decide(mesh);
  EXPECT_EQ(out.status, Status::Disconnected);
  EXPECT_FALSE(out.show_retry);
  InCallSignals hop;  // a stale seat failure while the hop path carries media: still connected
  hop.seat_media = MediaState::Failed;
  hop.media_chrome_live = true;
  hop.was_connected = true;
  EXPECT_EQ(Decide(hop).status, Status::Connected);
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
  s.soft_migrate_in_flight = true;  // a latch only: HopAttaching still owns the line
  EXPECT_EQ(Decide(s).status, Status::Connecting);
  s.media_status = CallMediaStatus::DirectConnecting;  // the latch shows once nothing else does
  EXPECT_EQ(Decide(s).status, Status::SettingUpGroup);
  s.soft_migrate_in_flight = false;
  s.sfu_attach_wait_active = true;
  EXPECT_EQ(Decide(s).status, Status::WaitingForMediaPath);
}

TEST(CallScreenTest, AFailedPathCouldNotConnectWithoutAHint) {
  InCallSignals s;
  s.seat_media = MediaState::Failed;
  InCallStatus out = Decide(s);
  EXPECT_EQ(out.status, Status::CouldntConnect);
  EXPECT_EQ(out.hint, CallScreen::Hint::None);
  EXPECT_TRUE(out.show_invite);
  s = {};
  s.media_status = CallMediaStatus::Failed;
  EXPECT_EQ(Decide(s).status, Status::CouldntConnect);
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
  s.awaiting_sfu_recovery = true;  // the group path is being recovered: before the engine state
  EXPECT_EQ(Decide(s).status, Status::Reconnecting);

  InCallSignals idle_active;  // media running, nothing else known
  idle_active.media_active = false;
  idle_active.seat_media = MediaState::Connecting;
  EXPECT_EQ(Decide(idle_active).status, Status::Connecting);
}

} // namespace
} // namespace pbr
