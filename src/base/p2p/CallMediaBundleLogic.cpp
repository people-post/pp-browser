#include "base/p2p/CallMediaBundleLogic.h"

namespace pbr {

CallMediaInboundHelloDecision DecideCallMediaInboundHello(const CallMediaInboundHelloContext& ctx) {
  if (ctx.other_bundle_busy) {
    return CallMediaInboundHelloDecision::RejectBusy;
  }
  switch (ctx.phase) {
  case CallMediaBundlePhase::MediaReady:
  case CallMediaBundlePhase::AwaitingMedia:
  case CallMediaBundlePhase::InboundHello:
  case CallMediaBundlePhase::Closing:
    return CallMediaInboundHelloDecision::RejectBusy;
  case CallMediaBundlePhase::OutboundHello:
    if (ctx.offerer && ctx.local_wins_glare) {
      return CallMediaInboundHelloDecision::RejectGlare;
    }
    // Loser (or outbound not bound yet): accept inbound and drop any provisional outbound.
    if (ctx.offerer && !ctx.local_wins_glare) {
      return CallMediaInboundHelloDecision::AcceptAndYield;
    }
    if (ctx.has_outbound_control) {
      return CallMediaInboundHelloDecision::AcceptAndYield;
    }
    return CallMediaInboundHelloDecision::Accept;
  case CallMediaBundlePhase::Idle:
    return CallMediaInboundHelloDecision::Accept;
  }
  return CallMediaInboundHelloDecision::RejectBusy;
}

CallMediaHelloAckDecision DecideCallMediaHelloAck(const CallMediaHelloAckContext& ctx) {
  if (!ctx.from_outbound_control) {
    return CallMediaHelloAckDecision::IgnoreStale;
  }
  if (ctx.phase == CallMediaBundlePhase::InboundHello || ctx.phase == CallMediaBundlePhase::Idle ||
      ctx.phase == CallMediaBundlePhase::Closing || ctx.phase == CallMediaBundlePhase::MediaReady) {
    return CallMediaHelloAckDecision::IgnoreStale;
  }
  if (ctx.phase != CallMediaBundlePhase::OutboundHello && ctx.phase != CallMediaBundlePhase::AwaitingMedia) {
    return CallMediaHelloAckDecision::IgnoreStale;
  }
  if (ctx.ack_ok) {
    return CallMediaHelloAckDecision::ProceedToMedia;
  }
  // Dual-dial: peer may RejectGlare our outbound even when identities were briefly unknown.
  // Yield so inbound adopt can proceed instead of failing the leg.
  if (ctx.phase == CallMediaBundlePhase::OutboundHello && ctx.offerer) {
    return CallMediaHelloAckDecision::YieldOutbound;
  }
  return CallMediaHelloAckDecision::Fail;
}

CallMediaChannelCloseDecision DecideCallMediaChannelClose(const CallMediaChannelCloseContext& ctx) {
  if (!ctx.slot_still_owned) {
    return CallMediaChannelCloseDecision::Ignore;
  }
  if (ctx.phase == CallMediaBundlePhase::Idle) {
    return CallMediaChannelCloseDecision::Ignore;
  }
  if (ctx.local_cancel || ctx.phase == CallMediaBundlePhase::Closing) {
    return CallMediaChannelCloseDecision::SuppressNotify;
  }
  // Dual-dial: peer abandoned their outbound (our inbound) after we won glare, or we rejected
  // inbound — must not tear down the winning outbound control.
  if (ctx.role == CallMediaChannelRole::InboundControl && ctx.phase == CallMediaBundlePhase::OutboundHello) {
    return CallMediaChannelCloseDecision::Ignore;
  }
  if (ctx.remote_terminal || ctx.role == CallMediaChannelRole::Media ||
      ctx.role == CallMediaChannelRole::InboundControl ||
      ctx.role == CallMediaChannelRole::OutboundControl) {
    return CallMediaChannelCloseDecision::FailLeg;
  }
  return CallMediaChannelCloseDecision::Ignore;
}

CallMediaSessionPhase CallMediaBundlePhaseToSessionPhase(const CallMediaBundlePhase phase) {
  switch (phase) {
  case CallMediaBundlePhase::Idle:
    return CallMediaSessionPhase::Idle;
  case CallMediaBundlePhase::OutboundHello:
    return CallMediaSessionPhase::HelloOutbound;
  case CallMediaBundlePhase::InboundHello:
    return CallMediaSessionPhase::HelloInbound;
  case CallMediaBundlePhase::AwaitingMedia:
    return CallMediaSessionPhase::Adopting;
  case CallMediaBundlePhase::MediaReady:
    return CallMediaSessionPhase::MediaReady;
  case CallMediaBundlePhase::Closing:
    return CallMediaSessionPhase::Detaching;
  }
  return CallMediaSessionPhase::Idle;
}

CallMediaLegPhase CallMediaBundlePhaseToLegPhase(const CallMediaBundlePhase phase) {
  switch (phase) {
  case CallMediaBundlePhase::Idle:
  case CallMediaBundlePhase::Closing:
    return CallMediaLegPhase::Closed;
  case CallMediaBundlePhase::OutboundHello:
  case CallMediaBundlePhase::InboundHello:
    return CallMediaLegPhase::ControlHello;
  case CallMediaBundlePhase::AwaitingMedia:
    return CallMediaLegPhase::AwaitingMedia;
  case CallMediaBundlePhase::MediaReady:
    return CallMediaLegPhase::MediaReady;
  }
  return CallMediaLegPhase::Closed;
}

bool CallMediaBundlePhaseIsActive(const CallMediaBundlePhase phase) {
  return phase != CallMediaBundlePhase::Idle && phase != CallMediaBundlePhase::Closing;
}

} // namespace pbr
