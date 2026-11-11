// DEV/PLAN-call-ui-never-detached.md: "the engine has an open call", the ongoing-call service it keeps on, and the
// last-resort guard that hangs up a call the screen lost (3 lost frames spanning 5 s, pauses not counted).
#include "feature/presenters/CallUiAttachGuard.h"

#include <gtest/gtest.h>
#include <vector>

using namespace pbr;

namespace {

constexpr CallPhase kAllPhases[] = {
    CallPhase::Idle,         CallPhase::Ringing,      CallPhase::Accepting,       CallPhase::OutboundCalling,
    CallPhase::JoinedLocal,  CallPhase::MediaPending, CallPhase::MediaConnecting, CallPhase::InCall,
    CallPhase::ConnectFailed};

bool OpenPhase(const CallPhase phase) {
  return phase == CallPhase::OutboundCalling || phase == CallPhase::JoinedLocal || phase == CallPhase::MediaPending ||
         phase == CallPhase::MediaConnecting || phase == CallPhase::InCall;
}

CallUiAttachGuard::Input Lost(const int64_t at_ms, const std::string& call = "call:a") {
  return {.engine_open = true, .engine_call_id = call, .ui_call_id = "", .steady_ms = at_ms};
}

CallUiAttachGuard::Input Shown(const int64_t at_ms, const std::string& call = "call:a") {
  return {.engine_open = true, .engine_call_id = call, .ui_call_id = call, .steady_ms = at_ms};
}

CallUiAttachGuard::Input Closed(const int64_t at_ms) {
  return {.engine_open = false, .engine_call_id = "", .ui_call_id = "", .steady_ms = at_ms};
}

} // namespace

TEST(EngineHasOpenCallTest, EveryPhaseAvailableOrNotWithOrWithoutAnId) {
  for (const CallPhase phase : kAllPhases) {
    for (const bool available : {false, true}) {
      for (const std::string id : {"", "call:a"}) {
        const bool expected = available && !id.empty() && OpenPhase(phase);
        EXPECT_EQ(EngineHasOpenCall(available, id, phase), expected)
            << CallPhaseName(phase) << " available=" << available << " id=" << id;
      }
    }
  }
}

TEST(EngineHasOpenCallTest, RingingAcceptingIdleConnectFailedAndUnavailableAreNever) {
  for (const CallPhase phase : {CallPhase::Ringing, CallPhase::Accepting, CallPhase::Idle, CallPhase::ConnectFailed}) {
    EXPECT_FALSE(EngineHasOpenCall(true, "call:a", phase)) << CallPhaseName(phase);
  }
  EXPECT_FALSE(EngineHasOpenCall(false, "call:a", CallPhase::InCall));
}

TEST(OngoingCallWantedTest, TheScreensCallOrTheEnginesOpenCall) {
  EXPECT_FALSE(OngoingCallWanted(false, false));
  EXPECT_TRUE(OngoingCallWanted(true, false));
  EXPECT_TRUE(OngoingCallWanted(false, true)) << "the screen lost it: the notification and its Hang up stay";
  EXPECT_TRUE(OngoingCallWanted(true, true));
}

TEST(CallUiAttachGuardTest, ACallOnScreenIsNeverTouched) {
  CallUiAttachGuard guard;
  for (int64_t t = 0; t <= 60'000; t += 500) {
    const auto step = guard.Feed(Shown(t));
    EXPECT_FALSE(step.report_detached || step.hang_up || step.stop_again) << t;
  }
}

TEST(CallUiAttachGuardTest, TwoLostFramesNeverHangUp) {
  CallUiAttachGuard guard;
  EXPECT_FALSE(guard.Feed(Lost(0)).hang_up);
  EXPECT_FALSE(guard.Feed(Lost(2'500)).hang_up);
  EXPECT_FALSE(guard.Feed(Shown(3'000)).hang_up);
}

TEST(CallUiAttachGuardTest, ThreeLostFramesUnderFiveSecondsDoNot) {
  CallUiAttachGuard guard;
  EXPECT_FALSE(guard.Feed(Lost(0)).hang_up);
  EXPECT_FALSE(guard.Feed(Lost(1'000)).hang_up);
  EXPECT_FALSE(guard.Feed(Lost(2'000)).hang_up);
  EXPECT_FALSE(guard.Feed(Lost(4'999)).hang_up) << "4.999 s";
}

TEST(CallUiAttachGuardTest, ThreeLostFramesOverFiveSecondsHangUpOnce) {
  CallUiAttachGuard guard;
  EXPECT_FALSE(guard.Feed(Lost(0)).hang_up);
  EXPECT_FALSE(guard.Feed(Lost(2'500)).hang_up);
  const auto step = guard.Feed(Lost(5'000));
  EXPECT_TRUE(step.hang_up);
  EXPECT_EQ(step.call_id, "call:a");
  for (int64_t t = 5'500; t <= 30'000; t += 500) {
    EXPECT_FALSE(guard.Feed(Lost(t)).hang_up) << "hung up twice at " << t;
  }
}

TEST(CallUiAttachGuardTest, FoundAgainStartsTheCountAgain) {
  CallUiAttachGuard guard;
  (void)guard.Feed(Lost(0));
  (void)guard.Feed(Lost(2'000));
  (void)guard.Feed(Lost(4'000));
  EXPECT_FALSE(guard.Feed(Shown(4'500)).hang_up);
  EXPECT_FALSE(guard.Feed(Lost(5'000)).hang_up) << "the lost time before was cleared";
  EXPECT_FALSE(guard.Feed(Lost(7'500)).hang_up);
  EXPECT_FALSE(guard.Feed(Lost(9'999)).hang_up) << "4.999 s since it was lost again";
  EXPECT_TRUE(guard.Feed(Lost(10'000)).hang_up);
}

TEST(CallUiAttachGuardTest, TheEngineLettingGoClearsTheCount) {
  CallUiAttachGuard guard;
  (void)guard.Feed(Lost(0));
  (void)guard.Feed(Lost(2'000));
  (void)guard.Feed(Closed(2'500));
  EXPECT_FALSE(guard.Feed(Lost(3'000)).hang_up);
  EXPECT_FALSE(guard.Feed(Lost(5'000)).hang_up);
}

// The app paused 30 s (screen off, the process frozen): that gap is no time on screen.
TEST(CallUiAttachGuardTest, APauseAddsNoTimeAndStartsTheCountAgain) {
  CallUiAttachGuard guard;
  (void)guard.Feed(Lost(0));
  (void)guard.Feed(Lost(500));
  EXPECT_FALSE(guard.Feed(Lost(30'500)).hang_up) << "1st frame after the pause";
  EXPECT_FALSE(guard.Feed(Lost(31'000)).hang_up) << "2nd";
  EXPECT_FALSE(guard.Feed(Lost(31'500)).hang_up) << "3rd";
  int64_t t = 31'500;
  while (t < 35'000) {
    t += 500;
    EXPECT_FALSE(guard.Feed(Lost(t)).hang_up) << t;
  }
  EXPECT_TRUE(guard.Feed(Lost(35'500)).hang_up) << "5 s lost since the pause";
}

TEST(CallUiAttachGuardTest, ReportedOncePerCallAndAnotherCallCountsAfresh) {
  CallUiAttachGuard guard;
  EXPECT_TRUE(guard.Feed(Lost(0)).report_detached);
  EXPECT_FALSE(guard.Feed(Lost(500)).report_detached);
  (void)guard.Feed(Shown(1'000));
  EXPECT_FALSE(guard.Feed(Lost(1'500)).report_detached) << "the same call, lost again";
  (void)guard.Feed(Lost(2'000));
  const auto other = guard.Feed(Lost(2'500, "call:b"));
  EXPECT_TRUE(other.report_detached);
  EXPECT_EQ(other.call_id, "call:b");
  EXPECT_FALSE(other.hang_up);
  EXPECT_FALSE(guard.Feed(Lost(5'000, "call:b")).hang_up) << "call:a's lost time is not call:b's";
  EXPECT_FALSE(guard.Feed(Lost(7'499, "call:b")).hang_up);
  EXPECT_TRUE(guard.Feed(Lost(7'500, "call:b")).hang_up);
}

TEST(CallUiAttachGuardTest, AHungUpCallStillOpenIsStoppedAgainEveryTwoSecondsThreeTimes) {
  CallUiAttachGuard guard;
  (void)guard.Feed(Lost(0));
  (void)guard.Feed(Lost(2'500));
  ASSERT_TRUE(guard.Feed(Lost(5'000)).hang_up);
  std::vector<int64_t> stops;
  for (int64_t t = 5'500; t <= 20'000; t += 500) {
    // Leave clears the screen's id; the presenter takes the engine's call back — both read "still open".
    const auto step = guard.Feed(t % 1'000 == 0 ? Shown(t) : Lost(t));
    EXPECT_FALSE(step.hang_up);
    EXPECT_FALSE(step.report_detached);
    if (step.stop_again) {
      EXPECT_EQ(step.call_id, "call:a");
      stops.push_back(t);
    }
  }
  EXPECT_EQ(stops, (std::vector<int64_t>{7'000, 9'000, 11'000}));
}

TEST(CallUiAttachGuardTest, AHungUpCallThatClosesNeedsNoMoreStops) {
  CallUiAttachGuard guard;
  (void)guard.Feed(Lost(0));
  (void)guard.Feed(Lost(2'500));
  ASSERT_TRUE(guard.Feed(Lost(5'000)).hang_up);
  (void)guard.Feed(Closed(5'100));
  for (int64_t t = 5'500; t <= 20'000; t += 500) {
    const auto step = guard.Feed(Closed(t));
    EXPECT_FALSE(step.stop_again || step.hang_up) << t;
  }
}

TEST(CallUiAttachGuardTest, NoOpenCallNeverTriggers) {
  // Ringing / Accepting / unavailable: EngineHasOpenCall is false, whatever the screen's id.
  CallUiAttachGuard guard;
  for (int64_t t = 0; t <= 60'000; t += 500) {
    const CallUiAttachGuard::Input in{.engine_open = EngineHasOpenCall(true, "call:ring", CallPhase::Ringing),
                                      .engine_call_id = "call:ring",
                                      .ui_call_id = "",
                                      .steady_ms = t};
    const auto step = guard.Feed(in);
    EXPECT_FALSE(step.report_detached || step.hang_up || step.stop_again) << t;
  }
}
