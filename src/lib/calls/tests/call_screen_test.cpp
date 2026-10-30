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

CallScreen GroupScreen(Status status, bool media_connected, bool was_connected) {
  CallScreen screen;
  screen.kind = CallScreen::Kind::InCall;
  screen.call_id = "call:g";
  screen.group = true;
  screen.members.resize(3);
  screen.status = status;
  screen.media_connected = media_connected;
  screen.was_connected = was_connected;
  return screen;
}

TEST(CallScreenTest, AGroupKeepsItsParticipantCountWhileItSetsItselfUpAgain) {
  // B102: someone rejoined — the media path moves (setting up / waiting for the hop) and the count stayed hidden.
  EXPECT_TRUE(ShowsParticipantCount(GroupScreen(Status::Connected, true, true)));
  EXPECT_TRUE(ShowsParticipantCount(GroupScreen(Status::SettingUpGroup, false, true)));
  EXPECT_TRUE(ShowsParticipantCount(GroupScreen(Status::WaitingForMediaPath, false, true)));
  EXPECT_TRUE(ShowsParticipantCount(GroupScreen(Status::Connecting, false, true)));
}

TEST(CallScreenTest, TheCountGivesWayToWhatTheUserMustRead) {
  EXPECT_FALSE(ShowsParticipantCount(GroupScreen(Status::Activity, true, true)));
  EXPECT_FALSE(ShowsParticipantCount(GroupScreen(Status::Reconnecting, false, true)));
  EXPECT_FALSE(ShowsParticipantCount(GroupScreen(Status::CouldntConnect, false, true)));
  EXPECT_FALSE(ShowsParticipantCount(GroupScreen(Status::SettingUpGroup, false, false))) << "never live yet";
  CallScreen alone = GroupScreen(Status::Connected, true, true);
  alone.members.clear();
  EXPECT_FALSE(ShowsParticipantCount(alone));
  CallScreen one_to_one = GroupScreen(Status::Connected, true, true);
  one_to_one.group = false;
  EXPECT_FALSE(ShowsParticipantCount(one_to_one));
}

TEST(CallScreenTest, ACouldntConnectCallThatMigratesAgainReadsConnecting) {
  InCallSignals s;
  s.p2p_connect_failed = true;
  s.media_active = true;
  s.group_context = true;
  EXPECT_EQ(Decide(s).status, Status::CouldntConnect);
  s.media_status = CallMediaStatus::HopAttaching;
  EXPECT_EQ(Decide(s).status, Status::Connecting);
  EXPECT_FALSE(Decide(s).show_retry);
  s.media_status = CallMediaStatus::HopWaiting;
  EXPECT_EQ(Decide(s).status, Status::WaitingForMediaPath);
  s.media_status = CallMediaStatus::Migrating;
  EXPECT_EQ(Decide(s).status, Status::SettingUpGroup);
  s.media_status = CallMediaStatus::None;
  EXPECT_EQ(Decide(s).status, Status::CouldntConnect) << "when it stops moving, the latch is still there";
}

TEST(CallScreenTest, ACallThatHungUpByItselfBeforeConnectingIsReported) {
  const CallScreen stuck = GroupScreen(Status::WaitingForMediaPath, false, false);
  const CallScreen gone;
  EXPECT_TRUE(EndedWithoutConnecting(stuck, gone, false));
  EXPECT_TRUE(EndedWithoutConnecting(GroupScreen(Status::CouldntConnect, false, false), gone, false));
  EXPECT_FALSE(EndedWithoutConnecting(stuck, gone, true)) << "the others ended it";
  EXPECT_FALSE(EndedWithoutConnecting(GroupScreen(Status::Connected, true, true), gone, false)) << "it was live";
  EXPECT_FALSE(EndedWithoutConnecting(GroupScreen(Status::Calling, false, false), gone, false)) << "unanswered, not stuck";
  EXPECT_FALSE(EndedWithoutConnecting(stuck, stuck, false)) << "still on screen";
  EXPECT_FALSE(EndedWithoutConnecting(CallScreen{}, gone, false));
}

// --- B97: a live group call where nothing arrives from anyone ends after 30 s --------------------

NoMediaSample LiveGroup(const int64_t now, const int64_t last_rx) {
  NoMediaSample sample;
  sample.call_id = "call:g";
  sample.group = true;
  sample.media_live = true;
  sample.others_joined = 2;
  sample.last_rx_ms = last_rx;
  sample.now_ms = now;
  return sample;
}

/** Steps once a second from `from` to `to` (inclusive); true as soon as a step says end. */
bool StepEverySecond(NoMediaWatch& watch, int64_t from, const int64_t to, NoMediaSample sample) {
  for (; from <= to; from += 1'000) {
    sample.now_ms = from;
    if (StepNoMediaWatch(watch, sample)) {
      return true;
    }
  }
  return false;
}

TEST(NoMediaWatchTest, NothingFromAnyoneForThirtySecondsEnds) {
  NoMediaWatch watch;
  EXPECT_FALSE(StepEverySecond(watch, 10'000, 39'000, LiveGroup(0, 5'000)));
  EXPECT_TRUE(StepNoMediaWatch(watch, LiveGroup(40'000, 5'000)));
}

TEST(NoMediaWatchTest, MutedPeersStillSendSoTheCallGoesOn) {
  NoMediaWatch watch;
  for (int64_t now = 10'000; now <= 120'000; now += 1'000) {
    EXPECT_FALSE(StepNoMediaWatch(watch, LiveGroup(now, now - 20))) << "silent frames keep arriving";
  }
}

TEST(NoMediaWatchTest, SomethingArrivingStartsTheCountOver) {
  NoMediaWatch watch;
  EXPECT_FALSE(StepEverySecond(watch, 10'000, 35'000, LiveGroup(0, 9'000)));
  EXPECT_FALSE(StepNoMediaWatch(watch, LiveGroup(36'000, 35'500)));
  EXPECT_FALSE(StepEverySecond(watch, 37'000, 66'000, LiveGroup(0, 35'500)));
  EXPECT_TRUE(StepNoMediaWatch(watch, LiveGroup(68'000, 35'500)));
}

TEST(NoMediaWatchTest, MovingOrReconnectingPausesTheCount) {
  NoMediaWatch watch;
  EXPECT_FALSE(StepEverySecond(watch, 10'000, 30'000, LiveGroup(0, 9'000)));
  NoMediaSample moving = LiveGroup(0, 9'000);
  moving.media_live = false;
  EXPECT_FALSE(StepEverySecond(watch, 31'000, 90'000, moving));
  EXPECT_FALSE(StepEverySecond(watch, 91'000, 120'000, LiveGroup(0, 9'000))) << "counted from 0 again";
  EXPECT_TRUE(StepNoMediaWatch(watch, LiveGroup(121'000, 9'000)));
}

TEST(NoMediaWatchTest, TheAppAsleepDoesNotCount) {
  NoMediaWatch watch;
  EXPECT_FALSE(StepEverySecond(watch, 10'000, 20'000, LiveGroup(0, 9'000)));
  // Backgrounded for 40 s: no refresh ran. Back in front, the count starts from 0.
  EXPECT_FALSE(StepNoMediaWatch(watch, LiveGroup(60'000, 9'000)));
  EXPECT_FALSE(StepEverySecond(watch, 61'000, 89'000, LiveGroup(0, 9'000)));
  EXPECT_TRUE(StepNoMediaWatch(watch, LiveGroup(91'000, 9'000)));
}

TEST(NoMediaWatchTest, OnlyAGroupCallWithOthersInItCounts) {
  NoMediaWatch watch;
  NoMediaSample one_to_one = LiveGroup(0, 9'000);
  one_to_one.group = false;
  EXPECT_FALSE(StepEverySecond(watch, 10'000, 100'000, one_to_one));
  NoMediaSample alone = LiveGroup(0, 9'000);
  alone.others_joined = 0;  // waiting alone: V053's own wait applies
  EXPECT_FALSE(StepEverySecond(watch, 101'000, 200'000, alone));
}

TEST(NoMediaWatchTest, AnotherCallStartsAFreshCount) {
  NoMediaWatch watch;
  EXPECT_FALSE(StepEverySecond(watch, 10'000, 35'000, LiveGroup(0, 9'000)));
  NoMediaSample other = LiveGroup(36'000, 9'000);
  other.call_id = "call:h";
  EXPECT_FALSE(StepEverySecond(watch, 36'000, 65'000, other));
  other.now_ms = 66'000;
  EXPECT_TRUE(StepNoMediaWatch(watch, other));
}

// --- V053: waiting alone in a group call ------------------------------------------------------------

CallScreen WaitingAlone(const int64_t until) {
  CallScreen s;
  s.kind = CallScreen::Kind::InCall;
  s.call_id = "call:g";
  s.group = true;
  s.media_connected = true;
  s.members = {{"me", true}};
  s.status = Status::WaitingForOthers;
  s.alone_until_ms = until;
  return s;
}

TEST(WaitingAloneTest, TheWaitLineIsNotHiddenByTheParticipantCount) {
  EXPECT_FALSE(ShowsParticipantCount(WaitingAlone(120'000)));
}

TEST(WaitingAloneTest, TheCallGoingAwayAtTheEndOfTheWaitIsToldOnce) {
  const CallScreen gone;
  EXPECT_TRUE(EndedAlone(WaitingAlone(120'000), gone, 120'400));
  EXPECT_FALSE(EndedAlone(WaitingAlone(120'000), gone, 60'000)) << "the user's own End, early";
  EXPECT_FALSE(EndedAlone(WaitingAlone(120'000), WaitingAlone(120'000), 121'000)) << "still in it";
  CallScreen live = WaitingAlone(0);
  live.status = Status::Connected;
  EXPECT_FALSE(EndedAlone(live, gone, 200'000)) << "was not waiting";
}

} // namespace
} // namespace pbr
