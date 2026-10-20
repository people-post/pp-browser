#include "feature/calls/LiveCall.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

// V037: what the device shows (State = phase, Status = the call's media) is projected from its calls.
class CallLifecycleTest : public ::testing::Test {
protected:
  void Answered(const std::string& call_id) {
    live_.AdmitInvited(call_id, {"account:peer"});
    live_.MarkAccepting(call_id);
    live_.MarkJoined(call_id);
  }
  LiveCalls live_;
};

TEST_F(CallLifecycleTest, AnswererRingsAcceptsWaitsForTheKeyAndGoesLive) {
  EXPECT_EQ(live_.Phase(), CallPhase::Idle);
  live_.AdmitInvited("call:a", {"account:peer"});
  EXPECT_EQ(live_.Phase(), CallPhase::Ringing);
  EXPECT_EQ(live_.Status(), CallMediaStatus::None) << "a ring is not the active call";

  live_.MarkAccepting("call:a");
  EXPECT_EQ(live_.Phase(), CallPhase::Accepting);
  EXPECT_EQ(live_.AcceptingCallId(), "call:a");

  live_.MarkJoined("call:a");
  EXPECT_EQ(live_.Phase(), CallPhase::JoinedLocal);
  EXPECT_EQ(live_.Status(), CallMediaStatus::Deciding);
  EXPECT_EQ(live_.ArmedPlanner(), CallArmedPlanner::Lifecycle) << "deciding arms no path planner yet";
  EXPECT_FALSE(live_.AllowsDirectPath());
  EXPECT_FALSE(live_.AllowsHopPath());

  live_.NoteMediaDeferred("call:a");
  EXPECT_EQ(live_.Phase(), CallPhase::MediaPending);
  live_.NoteMediaKeyReady("call:a");
  EXPECT_EQ(live_.Phase(), CallPhase::MediaConnecting);
  EXPECT_EQ(live_.Status(), CallMediaStatus::DirectConnecting) << "key ready prefers Direct";
  EXPECT_TRUE(live_.AllowsDirectPath());

  live_.NoteMediaConnected("call:a");
  EXPECT_EQ(live_.Phase(), CallPhase::InCall);
  EXPECT_EQ(live_.Status(), CallMediaStatus::DirectLive);
  // k4: losing the path mid-call shows Reconnecting under InCall, never back to Connecting.
  live_.ReportDirectProgress("call:a", CallDirectPlannerPhase::Reconnecting);
  EXPECT_EQ(live_.Phase(), CallPhase::InCall);
  EXPECT_EQ(live_.Status(), CallMediaStatus::Reconnecting);
}

TEST_F(CallLifecycleTest, PlacedCallRingsOutUntilAnsweredAndKeepsARunningPath) {
  live_.AdmitPlaced("call:out", {"account:peer"});
  live_.NoteOutboundStarted("call:out");
  EXPECT_EQ(live_.Phase(), CallPhase::OutboundCalling);
  EXPECT_EQ(live_.Status(), CallMediaStatus::Deciding);

  // A fast accept can start the 1:1 path first: OutboundStarted must not regress it.
  live_.SetMediaStatus("call:out", CallMediaStatus::DirectConnecting, "test");
  live_.NoteOutboundStarted("call:out");
  EXPECT_EQ(live_.Status(), CallMediaStatus::DirectConnecting);
  EXPECT_EQ(live_.Phase(), CallPhase::OutboundCalling) << "nobody answered yet";

  live_.MarkJoined("call:out");  // the peer answered
  EXPECT_EQ(live_.Phase(), CallPhase::MediaConnecting);
  live_.NoteMediaConnected("call:out");
  EXPECT_EQ(live_.Phase(), CallPhase::InCall);
}

TEST_F(CallLifecycleTest, FailedCallStaysOpenAndARestartConnectsAgain) {
  Answered("call:f");
  live_.NoteMediaConnected("call:f");
  live_.NoteMediaFailed("call:f");
  EXPECT_EQ(live_.Phase(), CallPhase::ConnectFailed);
  EXPECT_EQ(live_.Status(), CallMediaStatus::Failed);
  EXPECT_FALSE(live_.AllowsDirectPath()) << "Failed arms nothing";
  ASSERT_NE(live_.Active(), nullptr) << "the call is still open";

  live_.SetMediaStatus("call:f", CallMediaStatus::DirectConnecting, "RetryClicked");
  EXPECT_EQ(live_.Phase(), CallPhase::MediaConnecting) << "a restart connects, not InCall from before";
  live_.NoteMediaConnected("call:f");
  EXPECT_EQ(live_.Phase(), CallPhase::InCall);
}

TEST_F(CallLifecycleTest, HopStatusesArmTheGroupPathOnly) {
  Answered("call:g");
  live_.ReportHopProgress("call:g", CallHopPlannerPhase::WaitingAttach);
  EXPECT_EQ(live_.Status(), CallMediaStatus::HopWaiting);
  EXPECT_TRUE(live_.AllowsHopPath());
  EXPECT_FALSE(live_.AllowsDirectPath());
  EXPECT_FALSE(live_.SoftMigrateMayArm()) << "no SoftMigrate over a hop in progress";
  EXPECT_EQ(live_.ArmedPlanner(), CallArmedPlanner::Topology);

  live_.NoteMediaConnected("call:g");
  EXPECT_EQ(live_.Status(), CallMediaStatus::HopLive) << "connected while on the hop is HopLive";
  EXPECT_EQ(live_.Phase(), CallPhase::InCall);
  live_.ReportHopProgress("call:g", CallHopPlannerPhase::Idle);
  EXPECT_EQ(live_.Status(), CallMediaStatus::HopLive) << "idle / stopping progress changes nothing";
}

TEST_F(CallLifecycleTest, DirectArmingRequestHonouredWhileSettingUpOnly) {
  live_.AdmitPlaced("call:o", {"account:peer"});
  live_.NoteOutboundStarted("call:o");
  live_.RequestDirectArming("call:o");
  EXPECT_EQ(live_.Status(), CallMediaStatus::Deciding) << "an unanswered outbound call does not arm";
  live_.MarkJoined("call:o");
  live_.RequestDirectArming("call:o");
  EXPECT_EQ(live_.Status(), CallMediaStatus::DirectConnecting);

  live_.Close("call:o", LiveCallEndReason::LocalLeave);
  Answered("call:i");
  live_.ReportHopProgress("call:i", CallHopPlannerPhase::Live);
  live_.RequestDirectArming("call:i");
  EXPECT_EQ(live_.Status(), CallMediaStatus::HopLive) << "an InCall call never re-arms Direct";
}

TEST_F(CallLifecycleTest, CancelGenerationBumpsOnADecisionAndOnClose) {
  const uint64_t start = live_.MediaCancelGen();
  Answered("call:c");  // Deciding
  const uint64_t deciding = live_.MediaCancelGen();
  EXPECT_GT(deciding, start);
  live_.SetMediaStatus("call:c", CallMediaStatus::Deciding, "test");
  EXPECT_EQ(live_.MediaCancelGen(), deciding) << "staying Deciding is not a new decision";
  live_.Close("call:c", LiveCallEndReason::RemoteEnded);
  EXPECT_GT(live_.MediaCancelGen(), deciding) << "late path work for a closed call aborts";
}

TEST_F(CallLifecycleTest, TheActiveCallIsShownOverARingAndTheRingAfterIt) {
  Answered("call:in");
  live_.NoteMediaConnected("call:in");
  live_.AdmitInvited("call:ring", {"account:other"});
  EXPECT_EQ(live_.Phase(), CallPhase::InCall) << "a second ring does not take over the screen";
  ASSERT_NE(live_.Shown(), nullptr);
  EXPECT_EQ(live_.Shown()->Id(), "call:in");

  live_.Close("call:in", LiveCallEndReason::RemoteEnded);
  EXPECT_EQ(live_.Phase(), CallPhase::Ringing);
  EXPECT_EQ(live_.Shown()->Id(), "call:ring");
  live_.Close("call:ring", LiveCallEndReason::Declined);
  EXPECT_EQ(live_.Phase(), CallPhase::Idle);
  EXPECT_EQ(live_.Shown(), nullptr);
}

TEST_F(CallLifecycleTest, EventsForClosedOrUnknownCallsChangeNothing) {
  Answered("call:x");
  live_.Close("call:x", LiveCallEndReason::LocalLeave);
  live_.NoteMediaConnected("call:x");
  live_.NoteMediaFailed("call:unknown");
  EXPECT_EQ(live_.Phase(), CallPhase::Idle);
  EXPECT_EQ(live_.Status(), CallMediaStatus::None);
  EXPECT_EQ(live_.Find("call:x")->MediaProgress().status, CallMediaStatus::Deciding) << "a closed call keeps its last";
}

TEST_F(CallLifecycleTest, EveryChangeToWhatIsShownIsAnnounced) {
  int changes = 0;
  live_.SetOnChanged([&]() { ++changes; });
  live_.AdmitInvited("call:n", {"account:peer"});
  EXPECT_EQ(changes, 1);
  live_.MarkAccepting("call:n");
  live_.MarkJoined("call:n");
  live_.NoteMediaKeyReady("call:n");
  live_.NoteMediaConnected("call:n");
  live_.Close("call:n", LiveCallEndReason::LocalLeave);
  EXPECT_EQ(changes, 6);
}

} // namespace
} // namespace pbr
