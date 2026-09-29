#include "domain/mesh/l4/call_media/CallMediaBundleLogic.h"
#include "domain/mesh/l4/call_media/CallMediaSessionLogic.h"

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

TEST(CallMediaBundleLogicTest, ChannelCloseIgnoresInboundDuringOutboundHello) {
  CallMediaChannelCloseContext ctx;
  ctx.phase = CallMediaBundlePhase::OutboundHello;
  ctx.role = CallMediaChannelRole::InboundControl;
  ctx.remote_terminal = true;
  ctx.slot_still_owned = true;
  EXPECT_EQ(DecideCallMediaChannelClose(ctx), CallMediaChannelCloseDecision::Ignore);
}

TEST(CallMediaBundleLogicTest, ChannelCloseIgnoresClearedSlot) {
  CallMediaChannelCloseContext ctx;
  ctx.phase = CallMediaBundlePhase::OutboundHello;
  ctx.slot_still_owned = false;
  EXPECT_EQ(DecideCallMediaChannelClose(ctx), CallMediaChannelCloseDecision::Ignore);
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

} // namespace
} // namespace pbr
