#include "base/p2p/CallMediaBundleLogic.h"
#include "base/p2p/CallMediaSessionLogic.h"

#include <gtest/gtest.h>

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

} // namespace
} // namespace pbr
