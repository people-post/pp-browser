#include "feature/messaging/CallLifecycle.h"

#include "base/runtime/AppRuntime.h"

#include <chrono>
#include <gtest/gtest.h>
#include <thread>

namespace pbr {
namespace {

class CallLifecycleTest : public ::testing::Test {
protected:
  CallLifecycle life_;
};

TEST_F(CallLifecycleTest, InviteSeenRingsAndWantsListen) {
  EXPECT_EQ(life_.Phase(), CallPhase::Idle);
  EXPECT_FALSE(life_.WantEphemeralListen());

  life_.Apply(CallLifecycleEvent::InviteSeen, "call:1");

  EXPECT_EQ(life_.Phase(), CallPhase::Ringing);
  EXPECT_EQ(life_.ActiveCallId(), "call:1");
  EXPECT_EQ(life_.LastRingCallId(), "call:1");
  EXPECT_TRUE(life_.WantEphemeralListen());
  EXPECT_FALSE(life_.ShouldSuppressRing("call:1"));
}

TEST_F(CallLifecycleTest, InviteClearedReturnsIdleAndDropsListen) {
  life_.Apply(CallLifecycleEvent::InviteSeen, "call:1");
  life_.Apply(CallLifecycleEvent::InviteCleared, "call:1");

  EXPECT_EQ(life_.Phase(), CallPhase::Idle);
  EXPECT_TRUE(life_.ActiveCallId().empty());
  EXPECT_FALSE(life_.WantEphemeralListen());
}

TEST_F(CallLifecycleTest, InviteClearedIgnoredOutsideRinging) {
  life_.Apply(CallLifecycleEvent::OutboundStarted, "call:out");
  life_.Apply(CallLifecycleEvent::InviteCleared, "call:other");

  EXPECT_EQ(life_.Phase(), CallPhase::OutboundCalling);
  EXPECT_EQ(life_.ActiveCallId(), "call:out");
}

TEST_F(CallLifecycleTest, AcceptSucceededMediaPathToInCall) {
  life_.Apply(CallLifecycleEvent::AcceptSucceeded, "call:1");
  EXPECT_EQ(life_.Phase(), CallPhase::JoinedLocal);

  life_.Apply(CallLifecycleEvent::MediaDeferred, "call:1");
  EXPECT_EQ(life_.Phase(), CallPhase::MediaPending);

  life_.Apply(CallLifecycleEvent::MediaKeyReady, "call:1");
  EXPECT_EQ(life_.Phase(), CallPhase::MediaConnecting);

  life_.Apply(CallLifecycleEvent::DirectConnected, "call:1");
  EXPECT_EQ(life_.Phase(), CallPhase::InCall);
  EXPECT_TRUE(life_.WantEphemeralListen());
}

TEST_F(CallLifecycleTest, MediaDeferredIgnoredFromIdle) {
  life_.Apply(CallLifecycleEvent::MediaDeferred, "call:1");
  EXPECT_EQ(life_.Phase(), CallPhase::Idle);
}

TEST_F(CallLifecycleTest, MediaKeyReadyFromJoinedLocalSkipsPending) {
  life_.Apply(CallLifecycleEvent::AcceptSucceeded, "call:1");
  life_.Apply(CallLifecycleEvent::MediaKeyReady, "call:1");
  EXPECT_EQ(life_.Phase(), CallPhase::MediaConnecting);
}

TEST_F(CallLifecycleTest, ConnectFailedThenRemoteEndedClears) {
  life_.Apply(CallLifecycleEvent::OutboundStarted, "call:1");
  life_.Apply(CallLifecycleEvent::ConnectFailedEvt, "call:1");
  EXPECT_EQ(life_.Phase(), CallPhase::ConnectFailed);
  EXPECT_TRUE(life_.WantEphemeralListen());

  life_.Apply(CallLifecycleEvent::RemoteEnded, "call:1");
  EXPECT_EQ(life_.Phase(), CallPhase::Idle);
  EXPECT_FALSE(life_.WantEphemeralListen());
}

TEST_F(CallLifecycleTest, ConnectFailedIgnoredFromIdle) {
  life_.Apply(CallLifecycleEvent::ConnectFailedEvt, "call:1");
  EXPECT_EQ(life_.Phase(), CallPhase::Idle);
}

TEST_F(CallLifecycleTest, InviteSeenDuringInCallDoesNotLeavePhase) {
  life_.Apply(CallLifecycleEvent::DirectConnected, "call:active");
  EXPECT_EQ(life_.Phase(), CallPhase::InCall);

  life_.Apply(CallLifecycleEvent::InviteSeen, "call:other");
  EXPECT_EQ(life_.Phase(), CallPhase::InCall);
  EXPECT_EQ(life_.ActiveCallId(), "call:active");
  EXPECT_EQ(life_.LastRingCallId(), "call:other");
}

TEST_F(CallLifecycleTest, AcceptFailedReturnsToRinging) {
  life_.Apply(CallLifecycleEvent::AcceptSucceeded, "call:1");
  // Simulate Accepting→failed by applying AcceptFailed (clears accepting id).
  life_.Apply(CallLifecycleEvent::AcceptFailed, "call:1");
  EXPECT_EQ(life_.Phase(), CallPhase::Ringing);
  EXPECT_EQ(life_.ActiveCallId(), "call:1");
  EXPECT_TRUE(life_.AcceptingCallId().empty());
  EXPECT_FALSE(life_.ShouldSuppressRing("call:1"));
}

TEST_F(CallLifecycleTest, DeclineClickedWithoutCallIdIgnored) {
  life_.Apply(CallLifecycleEvent::DeclineClicked, {});
  EXPECT_EQ(life_.Phase(), CallPhase::Idle);
}

TEST_F(CallLifecycleTest, LeaveClickedWithoutCallIdIgnored) {
  life_.Apply(CallLifecycleEvent::LeaveClicked, {});
  EXPECT_EQ(life_.Phase(), CallPhase::Idle);
}

TEST_F(CallLifecycleTest, RetryClickedIgnoredUnlessConnectFailed) {
  life_.Apply(CallLifecycleEvent::OutboundStarted, "call:1");
  life_.Apply(CallLifecycleEvent::RetryClicked, "call:1");
  EXPECT_EQ(life_.Phase(), CallPhase::OutboundCalling);
}

TEST_F(CallLifecycleTest, ListenDesireCallbackFiresOnPhaseEnterExit) {
  int want_true = 0;
  int want_false = 0;
  life_.SetOnListenDesireChanged([&](const bool want) {
    if (want) {
      ++want_true;
    } else {
      ++want_false;
    }
  });

  life_.Apply(CallLifecycleEvent::InviteSeen, "call:1");
  EXPECT_EQ(want_true, 1);
  EXPECT_EQ(want_false, 0);

  life_.Apply(CallLifecycleEvent::InviteCleared, "call:1");
  EXPECT_EQ(want_true, 1);
  EXPECT_EQ(want_false, 1);
}

TEST_F(CallLifecycleTest, AcceptClickedSuppressesRingUntilAcceptFails) {
  AppRuntime::Initialize();
  AppRuntime::InitializeUI();

  life_.Apply(CallLifecycleEvent::InviteSeen, "call:1");
  life_.Apply(CallLifecycleEvent::AcceptClicked, "call:1");

  EXPECT_EQ(life_.Phase(), CallPhase::Accepting);
  EXPECT_EQ(life_.AcceptingCallId(), "call:1");
  EXPECT_TRUE(life_.ShouldSuppressRing("call:1"));
  EXPECT_FALSE(life_.ShouldSuppressRing("call:other"));

  // Unbound sessions → AcceptInvite fails → AcceptFailed → Ringing.
  bool back_to_ringing = false;
  for (int i = 0; i < 200; ++i) {
    AppRuntime::RunUITasks();
    if (life_.Phase() == CallPhase::Ringing && life_.AcceptingCallId().empty()) {
      back_to_ringing = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  AppRuntime::RunUITasks();
  EXPECT_TRUE(back_to_ringing);
  EXPECT_FALSE(life_.ShouldSuppressRing("call:1"));
  EXPECT_FALSE(life_.LastError().empty());

  AppRuntime::Shutdown();
  AppRuntime::ShutdownUI();
}

TEST_F(CallLifecycleTest, AcceptClickedDedupesInFlight) {
  AppRuntime::Initialize();
  AppRuntime::InitializeUI();

  life_.Apply(CallLifecycleEvent::InviteSeen, "call:1");
  life_.Apply(CallLifecycleEvent::AcceptClicked, "call:1");
  EXPECT_EQ(life_.Phase(), CallPhase::Accepting);
  // Re-click before draining UI AcceptFailed — accepting_call_id_ still set.
  life_.Apply(CallLifecycleEvent::AcceptClicked, "call:1");
  EXPECT_EQ(life_.Phase(), CallPhase::Accepting);
  EXPECT_EQ(life_.AcceptingCallId(), "call:1");

  for (int i = 0; i < 200; ++i) {
    AppRuntime::RunUITasks();
    if (life_.AcceptingCallId().empty()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  AppRuntime::RunUITasks();

  AppRuntime::Shutdown();
  AppRuntime::ShutdownUI();
}

TEST_F(CallLifecycleTest, ClearBindingResetsPhaseAndListen) {
  life_.Apply(CallLifecycleEvent::InviteSeen, "call:1");
  life_.ClearBinding();
  EXPECT_EQ(life_.Phase(), CallPhase::Idle);
  EXPECT_TRUE(life_.ActiveCallId().empty());
  EXPECT_FALSE(life_.WantEphemeralListen());
}

} // namespace
} // namespace pbr
