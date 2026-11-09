#include "lib/calls/CallMediaBundleLogic.h"
#include "lib/calls/CallMediaSessionLogic.h"

#include <gtest/gtest.h>

#include <utility>

namespace pbr {
namespace {

TEST(CallMediaBundleLogicTest, InboundHelloYieldsWhenOutboundLosesGlare) {
  CallMediaInboundHelloContext ctx;
  ctx.phase = CallMediaBundlePhase::OutboundHello;
  ctx.has_outbound_control = true;
  ctx.offerer = true;
  ctx.local_wins_glare = false;
  EXPECT_EQ(DecideCallMediaInboundHello(ctx), CallMediaInboundHelloDecision::AcceptAndYield);
}

TEST(CallMediaBundleLogicTest, InboundHelloYieldsWhenLoserOutboundNotBoundYet) {
  CallMediaInboundHelloContext ctx;
  ctx.phase = CallMediaBundlePhase::OutboundHello;
  ctx.has_outbound_control = false;
  ctx.offerer = true;
  ctx.local_wins_glare = false;
  EXPECT_EQ(DecideCallMediaInboundHello(ctx), CallMediaInboundHelloDecision::AcceptAndYield);
}

TEST(CallMediaBundleLogicTest, InboundHelloRejectsGlareWhenOutboundWins) {
  CallMediaInboundHelloContext ctx;
  ctx.phase = CallMediaBundlePhase::OutboundHello;
  ctx.has_outbound_control = true;
  ctx.offerer = true;
  ctx.local_wins_glare = true;
  EXPECT_EQ(DecideCallMediaInboundHello(ctx), CallMediaInboundHelloDecision::RejectGlare);
}

// Simultaneous open over one link: both sides are in OutboundHello and receive the other's
// hello. Exactly one must RejectGlare and the other AcceptAndYield — for every role pairing and
// PeerId order. Dogfood / hard-lab COLD-DIRTY: offerer with the lower PeerId + answerer both
// yielded, both bundles closed "peer_close", both sides ConnectFailed.
TEST(CallMediaBundleLogicTest, SimultaneousOpenHasExactlyOneWinner) {
  const std::pair<const char*, const char*> ids[] = {{"QmA", "QmB"}, {"QmB", "QmA"}};
  for (const auto& [a_id, b_id] : ids) {
    for (const bool a_offerer : {true, false}) {
      for (const bool b_offerer : {true, false}) {
        auto decide = [](bool local_offerer, bool remote_offerer, const char* local, const char* remote) {
          CallMediaInboundHelloContext ctx;
          ctx.phase = CallMediaBundlePhase::OutboundHello;
          ctx.has_outbound_control = true;
          ctx.offerer = local_offerer;
          ctx.local_wins_glare = LocalWinsCallMediaGlareForRoles(local_offerer, remote_offerer, local, remote);
          return DecideCallMediaInboundHello(ctx);
        };
        const auto a = decide(a_offerer, b_offerer, a_id, b_id);
        const auto b = decide(b_offerer, a_offerer, b_id, a_id);
        const int rejects = (a == CallMediaInboundHelloDecision::RejectGlare) +
                            (b == CallMediaInboundHelloDecision::RejectGlare);
        const int yields = (a == CallMediaInboundHelloDecision::AcceptAndYield) +
                           (b == CallMediaInboundHelloDecision::AcceptAndYield);
        EXPECT_EQ(rejects, 1) << "a=" << a_id << (a_offerer ? "/offerer" : "/answerer") << " b=" << b_id
                              << (b_offerer ? "/offerer" : "/answerer");
        EXPECT_EQ(yields, 1);
      }
    }
  }
}

TEST(CallMediaBundleLogicTest, OffererBeatsAnswererRegardlessOfPeerId) {
  EXPECT_TRUE(LocalWinsCallMediaGlareForRoles(true, false, "QmA", "QmB"));
  EXPECT_FALSE(LocalWinsCallMediaGlareForRoles(false, true, "QmB", "QmA"));
  // Same role (e.g. both retried as offerer): PeerId order.
  EXPECT_TRUE(LocalWinsCallMediaGlareForRoles(true, true, "QmB", "QmA"));
  EXPECT_FALSE(LocalWinsCallMediaGlareForRoles(true, true, "QmA", "QmB"));
}

TEST(CallMediaBundleLogicTest, InboundHelloBusyWhenMediaReadyOrOtherBundle) {
  CallMediaInboundHelloContext ctx;
  ctx.phase = CallMediaBundlePhase::MediaReady;
  EXPECT_EQ(DecideCallMediaInboundHello(ctx), CallMediaInboundHelloDecision::RejectBusy);

  ctx.phase = CallMediaBundlePhase::Idle;
  ctx.other_bundle_busy = true;
  EXPECT_EQ(DecideCallMediaInboundHello(ctx), CallMediaInboundHelloDecision::RejectBusy);
}

TEST(CallMediaBundleLogicTest, HelloAckIgnoresStaleAfterYield) {
  CallMediaHelloAckContext ctx;
  ctx.phase = CallMediaBundlePhase::InboundHello;
  ctx.ack_ok = false;
  ctx.from_outbound_control = true;
  ctx.offerer = true;
  ctx.local_wins_glare = false;
  EXPECT_EQ(DecideCallMediaHelloAck(ctx), CallMediaHelloAckDecision::IgnoreStale);
}

TEST(CallMediaBundleLogicTest, HelloAckProceedsOnOutboundOk) {
  CallMediaHelloAckContext ctx;
  ctx.phase = CallMediaBundlePhase::OutboundHello;
  ctx.ack_ok = true;
  ctx.from_outbound_control = true;
  EXPECT_EQ(DecideCallMediaHelloAck(ctx), CallMediaHelloAckDecision::ProceedToMedia);
}

TEST(CallMediaBundleLogicTest, HelloAckYieldsOutboundOnOffererNack) {
  CallMediaHelloAckContext ctx;
  ctx.phase = CallMediaBundlePhase::OutboundHello;
  ctx.ack_ok = false;
  ctx.from_outbound_control = true;
  ctx.offerer = true;
  ctx.local_wins_glare = true; // even if local thought it won, yield for dual-dial recovery
  EXPECT_EQ(DecideCallMediaHelloAck(ctx), CallMediaHelloAckDecision::YieldOutbound);
}

// A closed channel is routed by the path that held it first: a placeholder's carries no call, a
// candidate's abandons the migration, a retiring one's is expected, a standby's loses the standby only.
TEST(CallMediaChannelCloseTest, TheClosedChannelsPathDecidesFirst) {
  CallMediaChannelCloseInput in;
  in.phase = CallMediaBundlePhase::MediaReady;
  in.remote_terminal = true;
  in.slot = CallMediaPathSlot::Candidate;
  EXPECT_EQ(RouteCallMediaChannelClose(in), CallMediaChannelCloseAction::AbandonCandidate);
  in.slot = CallMediaPathSlot::Retiring;
  EXPECT_EQ(RouteCallMediaChannelClose(in), CallMediaChannelCloseAction::DropRetiring);
  in.slot = CallMediaPathSlot::Standby;
  EXPECT_EQ(RouteCallMediaChannelClose(in), CallMediaChannelCloseAction::DropStandby);
  in.placeholder = true;
  in.phase = CallMediaBundlePhase::Idle;
  for (const auto slot : {CallMediaPathSlot::Active, CallMediaPathSlot::Candidate}) {
    in.slot = slot;
    EXPECT_EQ(RouteCallMediaChannelClose(in), CallMediaChannelCloseAction::DropPlaceholder)
        << "a placeholder's channel ended before hello / migrate";
  }
}

// K014: a live call's active path closing under it goes to the active-close rules (a lost path),
// whoever closed it and whichever channel — unless this end is ending the leg.
TEST(CallMediaChannelCloseTest, ALiveCallsActivePathClosingIsItsOwnCase) {
  CallMediaChannelCloseInput in;
  in.phase = CallMediaBundlePhase::MediaReady;
  for (const auto role :
       {CallMediaChannelRole::OutboundControl, CallMediaChannelRole::InboundControl, CallMediaChannelRole::Media}) {
    for (const bool remote : {true, false}) {
      in.role = role;
      in.remote_terminal = remote;
      EXPECT_EQ(RouteCallMediaChannelClose(in), CallMediaChannelCloseAction::ActivePathClosed);
    }
  }
  in.local_cancel = true;
  EXPECT_EQ(RouteCallMediaChannelClose(in), CallMediaChannelCloseAction::TearDownQuietly);
}

// A connecting leg: any channel closing fails it — reported only when the peer closed it; a dual-dial
// inbound that goes while our outbound wins glare drops that slot only.
TEST(CallMediaChannelCloseTest, AConnectingLegFailsOnAnyCloseAndReportsThePeers) {
  CallMediaChannelCloseInput in;
  in.phase = CallMediaBundlePhase::OutboundHello;
  in.role = CallMediaChannelRole::InboundControl;
  in.remote_terminal = true;
  EXPECT_EQ(RouteCallMediaChannelClose(in), CallMediaChannelCloseAction::DropInboundControl);
  for (const auto phase :
       {CallMediaBundlePhase::OutboundHello, CallMediaBundlePhase::InboundHello, CallMediaBundlePhase::AwaitingMedia}) {
    for (const auto role : {CallMediaChannelRole::OutboundControl, CallMediaChannelRole::Media}) {
      in.phase = phase;
      in.role = role;
      in.remote_terminal = true;
      EXPECT_EQ(RouteCallMediaChannelClose(in), CallMediaChannelCloseAction::TearDownReported);
      in.remote_terminal = false;
      EXPECT_EQ(RouteCallMediaChannelClose(in), CallMediaChannelCloseAction::TearDownQuietly);
    }
  }
  in.remote_terminal = true;
  in.local_cancel = true;
  EXPECT_EQ(RouteCallMediaChannelClose(in), CallMediaChannelCloseAction::TearDownQuietly) << "this end cancelled";
  in.local_cancel = false;
  in.phase = CallMediaBundlePhase::Closing;
  EXPECT_EQ(RouteCallMediaChannelClose(in), CallMediaChannelCloseAction::TearDownQuietly);
  in.phase = CallMediaBundlePhase::Idle;
  EXPECT_EQ(RouteCallMediaChannelClose(in), CallMediaChannelCloseAction::Ignore);
}

TEST(CallMediaBundleLogicTest, PhaseMapsMatchSessionVocabulary) {
  EXPECT_EQ(CallMediaBundlePhaseToSessionPhase(CallMediaBundlePhase::OutboundHello),
            CallMediaSessionPhase::HelloOutbound);
  EXPECT_EQ(CallMediaBundlePhaseToSessionPhase(CallMediaBundlePhase::MediaReady),
            CallMediaSessionPhase::MediaReady);
  EXPECT_EQ(CallMediaBundlePhaseToLegPhase(CallMediaBundlePhase::AwaitingMedia),
            CallMediaLegPhase::AwaitingMedia);
  EXPECT_TRUE(LocalWinsCallMediaGlare("b", "a"));
}

// k3: overlapping paths can deliver a frame twice; the receiver keeps the first copy only.
TEST(CallMediaSeqWindowTest, DropsDuplicatesAcrossReorderingAndRestarts) {
  CallMediaSeqWindow window;
  EXPECT_TRUE(window.Accept(10));
  EXPECT_FALSE(window.Accept(10)) << "same frame on the other path";
  EXPECT_TRUE(window.Accept(12));
  EXPECT_TRUE(window.Accept(11)) << "late but new";
  EXPECT_FALSE(window.Accept(11));
  EXPECT_TRUE(window.Accept(12 + CallMediaSeqWindow::kWindow - 1));
  EXPECT_FALSE(window.Accept(12)) << "still inside the window";
  EXPECT_TRUE(window.Accept(5000));
  EXPECT_TRUE(window.Accept(1)) << "far behind: the sender restarted its seq";
  EXPECT_FALSE(window.Accept(1));
  EXPECT_TRUE(window.Accept(2));
}

// k4: with nothing to fail over to, a lost link or 3 s of silence from a heartbeating peer is a
// lost path (Reconnecting…). The peer's app died: its link can stay "connected" for most of a minute.
TEST(CallMediaFailoverTest, ASilentPathWithNoStandbyIsLost) {
  CallMediaFailoverInput in;
  in.peer_heartbeats = true;
  EXPECT_FALSE(ShouldEnterPathLost(in)) << "healthy active";
  in.active_silence_ms = kCallMediaActiveSilenceLostMs - 1;
  EXPECT_FALSE(ShouldEnterPathLost(in));
  in.active_silence_ms = kCallMediaActiveSilenceLostMs;
  EXPECT_TRUE(ShouldEnterPathLost(in)) << "silent, no standby";
  in.peer_heartbeats = false;
  EXPECT_FALSE(ShouldEnterPathLost(in)) << "a peer that never heartbeats may just be quiet";
  in.active_link_lost = true;
  EXPECT_TRUE(ShouldEnterPathLost(in)) << "lost link";
  in.active_link_lost = false;
  in.peer_heartbeats = true;
  in.have_standby = true;
  in.standby_link_alive = true;
  EXPECT_FALSE(ShouldEnterPathLost(in)) << "fails over to the standby instead";
  in.standby_link_alive = false;
  EXPECT_TRUE(ShouldEnterPathLost(in)) << "the standby is gone too";
}

// k4: fail over only onto a live standby, on a lost active link or 1.5 s of silence from a peer that
// heartbeats (older peers never trip the silence rule).
TEST(CallMediaFailoverTest, SwitchesOnlyToALiveStandbyForALostOrSilentActive) {
  CallMediaFailoverInput in;
  in.have_standby = true;
  in.standby_link_alive = true;
  in.peer_heartbeats = true;
  EXPECT_FALSE(ShouldFailOverToStandby(in)) << "healthy active";
  in.active_silence_ms = kCallMediaActiveSilenceFailoverMs - 1;
  EXPECT_FALSE(ShouldFailOverToStandby(in));
  in.active_silence_ms = kCallMediaActiveSilenceFailoverMs;
  EXPECT_TRUE(ShouldFailOverToStandby(in)) << "silent active";
  in.since_failover_ms = kCallMediaFailoverHoldDownMs - 1;
  EXPECT_FALSE(ShouldFailOverToStandby(in)) << "just failed over: the peer may still be moving";
  in.since_failover_ms = INT64_MAX;
  in.active_silence_ms = 0;
  in.active_link_lost = true;
  in.since_failover_ms = 0;
  EXPECT_TRUE(ShouldFailOverToStandby(in)) << "lost active (no hold-down for a lost link)";
  in.since_failover_ms = INT64_MAX;
  in.standby_silence_ms = kCallMediaStandbyStaleMs;
  EXPECT_FALSE(ShouldFailOverToStandby(in)) << "stale standby";
  in.standby_silence_ms = 0;
  in.standby_link_alive = false;
  EXPECT_FALSE(ShouldFailOverToStandby(in)) << "standby gone";
  in.standby_link_alive = true;
  in.have_standby = false;
  EXPECT_FALSE(ShouldFailOverToStandby(in)) << "nothing to switch to";

  CallMediaFailoverInput older_peer;
  older_peer.have_standby = true;
  older_peer.standby_link_alive = true;
  older_peer.active_silence_ms = 60000;
  EXPECT_FALSE(ShouldFailOverToStandby(older_peer)) << "no heartbeats: silence proves nothing";
  older_peer.active_link_lost = true;
  EXPECT_TRUE(ShouldFailOverToStandby(older_peer));
}

// K014: while the active path waits for a `bye` (the peer closed its media channel first), silence
// switches nothing — the wait's end decides; a lost link still does at once.
TEST(CallMediaFailoverTest, AnAwaitedByeHoldsOffTheSilenceRules) {
  CallMediaFailoverInput in;
  in.peer_heartbeats = true;
  in.have_standby = true;
  in.standby_link_alive = true;
  in.active_silence_ms = kCallMediaActiveSilenceLostMs;
  in.awaiting_bye = true;
  EXPECT_FALSE(ShouldFailOverToStandby(in));
  in.have_standby = false;
  EXPECT_FALSE(ShouldEnterPathLost(in));
  in.active_link_lost = true;
  EXPECT_TRUE(ShouldEnterPathLost(in)) << "a lost link carries no bye";
  in.have_standby = true;
  EXPECT_TRUE(ShouldFailOverToStandby(in));
}

// K014: a close of a live call's active path that the peer did not announce with `bye` is a lost
// path (fall back, fail over, or reconnect) — never the call's end. Only a media channel closing
// under a still-open control channel waits for that channel's word: its best-effort CLOSE can
// overtake the reliable `bye`.
TEST(CallMediaActiveCloseTest, AnUnannouncedCloseIsALostPathNeverAHangUp) {
  CallMediaActiveCloseInput in;
  in.remote_close = true;
  in.role = CallMediaChannelRole::OutboundControl;
  in.control_open = true;
  EXPECT_EQ(DecideCallMediaActiveClose(in), CallMediaActiveCloseAction::PathLost) << "a bare peer_close";
  in.role = CallMediaChannelRole::InboundControl;
  EXPECT_EQ(DecideCallMediaActiveClose(in), CallMediaActiveCloseAction::PathLost);
  in.role = CallMediaChannelRole::Media;
  EXPECT_EQ(DecideCallMediaActiveClose(in), CallMediaActiveCloseAction::AwaitControl)
      << "the bye may still be on its way on the control channel";
  in.control_open = false;
  EXPECT_EQ(DecideCallMediaActiveClose(in), CallMediaActiveCloseAction::PathLost);
  in.control_open = true;
  in.remote_close = false;
  EXPECT_EQ(DecideCallMediaActiveClose(in), CallMediaActiveCloseAction::PathLost)
      << "the link went: nothing to wait for";

  in.role = CallMediaChannelRole::OutboundControl;
  in.remote_close = true;
  in.standby_alive = true;
  EXPECT_EQ(DecideCallMediaActiveClose(in), CallMediaActiveCloseAction::FailOverToStandby)
      << "a peer_close fails over like a dead link";
  in.retiring_alive = true;
  EXPECT_EQ(DecideCallMediaActiveClose(in), CallMediaActiveCloseAction::FallBackToRetiring);
}

} // namespace
} // namespace pbr

// PB-018: a hello from a PeerId other than the one the call's bundle is bound to is refused in every
// phase — above all when we dial and lose glare, where a stranger's hello would pass for the answer.
TEST(CallMediaBundleLogicTest, AHelloFromAnotherPeerIsRefusedInEveryPhase) {
  using namespace pbr;
  for (const CallMediaBundlePhase phase :
       {CallMediaBundlePhase::Idle, CallMediaBundlePhase::OutboundHello, CallMediaBundlePhase::InboundHello,
        CallMediaBundlePhase::AwaitingMedia, CallMediaBundlePhase::MediaReady}) {
    for (const bool local_wins : {true, false}) {
      CallMediaInboundHelloContext ctx;
      ctx.phase = phase;
      ctx.has_outbound_control = phase == CallMediaBundlePhase::OutboundHello;
      ctx.local_wins_glare = local_wins;
      ctx.from_another_peer = true;
      EXPECT_EQ(DecideCallMediaInboundHello(ctx), CallMediaInboundHelloDecision::RejectPeer)
          << static_cast<int>(phase) << " wins=" << local_wins;
    }
  }
  CallMediaInboundHelloContext same_peer;
  same_peer.phase = CallMediaBundlePhase::OutboundHello;
  same_peer.has_outbound_control = true;
  same_peer.local_wins_glare = false;
  EXPECT_EQ(DecideCallMediaInboundHello(same_peer), CallMediaInboundHelloDecision::AcceptAndYield)
      << "the peer itself still wins glare as before";
}
