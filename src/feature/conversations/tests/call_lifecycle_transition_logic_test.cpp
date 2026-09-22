#include "domain/messaging/CallLifecycleTransitionLogic.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

CallLifecycleTransitionContext Ctx(CallPhase phase,
                                   CallMediaStatus status = CallMediaStatus::None) {
  CallLifecycleTransitionContext ctx;
  ctx.phase = phase;
  ctx.status = status;
  return ctx;
}

TEST(CallLifecycleTransitionLogicTest, InviteSeenIdleToRinging) {
  auto ctx = Ctx(CallPhase::Idle);
  ctx.event_call_id = "call:1";
  const auto out = DecideCallLifecycleTransition(CallLifecycleEvent::InviteSeen, ctx);
  EXPECT_TRUE(HasAction(out.actions, CallLifecycleAction::SetPhase));
  EXPECT_EQ(out.next_phase, CallPhase::Ringing);
  EXPECT_TRUE(HasAction(out.actions, CallLifecycleAction::NoteRing));
  EXPECT_TRUE(HasAction(out.actions, CallLifecycleAction::NotifyChrome));
}

TEST(CallLifecycleTransitionLogicTest, InviteSeenWhileInCallNotesRingOnly) {
  auto ctx = Ctx(CallPhase::InCall, CallMediaStatus::DirectLive);
  ctx.event_call_id = "call:2";
  const auto out = DecideCallLifecycleTransition(CallLifecycleEvent::InviteSeen, ctx);
  EXPECT_FALSE(HasAction(out.actions, CallLifecycleAction::SetPhase));
  EXPECT_TRUE(HasAction(out.actions, CallLifecycleAction::NoteRing));
  EXPECT_TRUE(HasAction(out.actions, CallLifecycleAction::NotifyChrome));
}

TEST(CallLifecycleTransitionLogicTest, AcceptClickedRequiresCallId) {
  auto ctx = Ctx(CallPhase::Ringing);
  const auto out = DecideCallLifecycleTransition(CallLifecycleEvent::AcceptClicked, ctx);
  EXPECT_TRUE(HasAction(out.actions, CallLifecycleAction::LogIgnored));
  EXPECT_FALSE(HasAction(out.actions, CallLifecycleAction::PostAcceptInvite));
}

TEST(CallLifecycleTransitionLogicTest, AcceptClickedFromLastRing) {
  auto ctx = Ctx(CallPhase::Ringing);
  ctx.last_ring_call_id = "call:ring";
  const auto out = DecideCallLifecycleTransition(CallLifecycleEvent::AcceptClicked, ctx);
  EXPECT_EQ(out.call_id, "call:ring");
  EXPECT_EQ(out.next_phase, CallPhase::Accepting);
  EXPECT_TRUE(HasAction(out.actions, CallLifecycleAction::PostAcceptInvite));
  EXPECT_TRUE(HasAction(out.actions, CallLifecycleAction::DeferChrome));
}

TEST(CallLifecycleTransitionLogicTest, AcceptClickedAlreadyInFlight) {
  auto ctx = Ctx(CallPhase::Accepting);
  ctx.accepting_call_id = "call:a";
  ctx.event_call_id = "call:a";
  const auto out = DecideCallLifecycleTransition(CallLifecycleEvent::AcceptClicked, ctx);
  EXPECT_TRUE(HasAction(out.actions, CallLifecycleAction::LogIgnored));
  EXPECT_TRUE(HasAction(out.actions, CallLifecycleAction::NotifyChrome));
  EXPECT_FALSE(HasAction(out.actions, CallLifecycleAction::PostAcceptInvite));
}

TEST(CallLifecycleTransitionLogicTest, AcceptSucceededStaleIgnored) {
  auto ctx = Ctx(CallPhase::Accepting);
  ctx.active_call_id = "call:b";
  ctx.event_call_id = "call:a";
  const auto out = DecideCallLifecycleTransition(CallLifecycleEvent::AcceptSucceeded, ctx);
  EXPECT_TRUE(HasAction(out.actions, CallLifecycleAction::LogIgnored));
  EXPECT_FALSE(HasAction(out.actions, CallLifecycleAction::SetPhase));
}

TEST(CallLifecycleTransitionLogicTest, AcceptSucceededKeepsMediaConnecting) {
  auto ctx = Ctx(CallPhase::MediaConnecting, CallMediaStatus::DirectConnecting);
  ctx.active_call_id = "call:1";
  ctx.event_call_id = "call:1";
  ctx.sessions_bound = true;
  ctx.allows_direct_path = true;
  const auto out = DecideCallLifecycleTransition(CallLifecycleEvent::AcceptSucceeded, ctx);
  EXPECT_FALSE(HasAction(out.actions, CallLifecycleAction::SetPhase));
  EXPECT_TRUE(HasAction(out.actions, CallLifecycleAction::LogKeepPhase));
  EXPECT_TRUE(HasAction(out.actions, CallLifecycleAction::KickAnswererDirectMedia));
}

TEST(CallLifecycleTransitionLogicTest, AcceptSucceededToJoinedLocal) {
  auto ctx = Ctx(CallPhase::Accepting);
  ctx.event_call_id = "call:1";
  const auto out = DecideCallLifecycleTransition(CallLifecycleEvent::AcceptSucceeded, ctx);
  EXPECT_TRUE(HasAction(out.actions, CallLifecycleAction::SetPhase));
  EXPECT_EQ(out.next_phase, CallPhase::JoinedLocal);
  EXPECT_FALSE(HasAction(out.actions, CallLifecycleAction::KickAnswererDirectMedia));
}

TEST(CallLifecycleTransitionLogicTest, DeclineIdleAndPost) {
  auto ctx = Ctx(CallPhase::Ringing);
  ctx.event_call_id = "call:d";
  const auto out = DecideCallLifecycleTransition(CallLifecycleEvent::DeclineClicked, ctx);
  EXPECT_EQ(out.next_phase, CallPhase::Idle);
  EXPECT_TRUE(HasAction(out.actions, CallLifecycleAction::PostDeclineInvite));
}

TEST(CallLifecycleTransitionLogicTest, MediaDeferredFromJoinedLocal) {
  auto ctx = Ctx(CallPhase::JoinedLocal);
  ctx.event_call_id = "call:1";
  const auto out = DecideCallLifecycleTransition(CallLifecycleEvent::MediaDeferred, ctx);
  EXPECT_EQ(out.next_phase, CallPhase::MediaPending);
}

TEST(CallLifecycleTransitionLogicTest, MediaDeferredIgnoredFromIdle) {
  auto ctx = Ctx(CallPhase::Idle);
  const auto out = DecideCallLifecycleTransition(CallLifecycleEvent::MediaDeferred, ctx);
  EXPECT_EQ(out.actions, CallLifecycleAction::None);
}

TEST(CallLifecycleTransitionLogicTest, DirectConnectedPrefersHopLive) {
  auto ctx = Ctx(CallPhase::MediaConnecting, CallMediaStatus::HopAttaching);
  ctx.event_call_id = "call:1";
  const auto out = DecideCallLifecycleTransition(CallLifecycleEvent::DirectConnected, ctx);
  EXPECT_EQ(out.next_phase, CallPhase::InCall);
  EXPECT_EQ(out.next_status, CallMediaStatus::HopLive);
}

TEST(CallLifecycleTransitionLogicTest, RemoteEndedStaleIgnored) {
  auto ctx = Ctx(CallPhase::InCall, CallMediaStatus::DirectLive);
  ctx.active_call_id = "call:live";
  ctx.event_call_id = "call:old";
  const auto out = DecideCallLifecycleTransition(CallLifecycleEvent::RemoteEnded, ctx);
  EXPECT_TRUE(HasAction(out.actions, CallLifecycleAction::LogIgnored));
  EXPECT_FALSE(HasAction(out.actions, CallLifecycleAction::SetPhase));
}

TEST(CallLifecycleTransitionLogicTest, ConnectFailedFromIdleNoOp) {
  auto ctx = Ctx(CallPhase::Idle);
  const auto out = DecideCallLifecycleTransition(CallLifecycleEvent::ConnectFailedEvt, ctx);
  EXPECT_EQ(out.actions, CallLifecycleAction::None);
}

TEST(CallLifecycleTransitionLogicTest, OutboundStartedArmsDecidingFromNone) {
  auto ctx = Ctx(CallPhase::Idle);
  ctx.event_call_id = "call:1";
  const auto out = DecideCallLifecycleTransition(CallLifecycleEvent::OutboundStarted, ctx);
  EXPECT_EQ(out.next_phase, CallPhase::OutboundCalling);
  EXPECT_TRUE(HasAction(out.actions, CallLifecycleAction::SetStatus));
  EXPECT_EQ(out.next_status, CallMediaStatus::Deciding);
}

TEST(CallLifecycleTransitionLogicTest, OutboundStartedDoesNotRegressDirectConnecting) {
  auto ctx = Ctx(CallPhase::OutboundCalling, CallMediaStatus::DirectConnecting);
  ctx.event_call_id = "call:1";
  const auto out = DecideCallLifecycleTransition(CallLifecycleEvent::OutboundStarted, ctx);
  EXPECT_EQ(out.next_phase, CallPhase::OutboundCalling);
  EXPECT_FALSE(HasAction(out.actions, CallLifecycleAction::SetStatus));
  EXPECT_EQ(out.next_status, CallMediaStatus::DirectConnecting);
}

TEST(CallLifecycleTransitionLogicTest, RetryOnlyFromConnectFailed) {
  auto ctx = Ctx(CallPhase::InCall);
  ctx.event_call_id = "call:1";
  auto out = DecideCallLifecycleTransition(CallLifecycleEvent::RetryClicked, ctx);
  EXPECT_TRUE(HasAction(out.actions, CallLifecycleAction::LogIgnored));

  ctx.phase = CallPhase::ConnectFailed;
  out = DecideCallLifecycleTransition(CallLifecycleEvent::RetryClicked, ctx);
  EXPECT_TRUE(HasAction(out.actions, CallLifecycleAction::PostRetryMedia));
}

} // namespace
} // namespace pbr
