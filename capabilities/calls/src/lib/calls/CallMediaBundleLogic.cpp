#include "lib/calls/CallMediaBundleLogic.h"

namespace pbr {

CallMediaInboundHelloDecision DecideCallMediaInboundHello(const CallMediaInboundHelloContext& ctx) {
  if (ctx.from_another_peer) {
    return CallMediaInboundHelloDecision::RejectPeer;
  }
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
      // Simultaneous open: exactly one side must win. local_wins_glare is role-aware and
      // antisymmetric (LocalWinsCallMediaGlareForRoles). The old rule let an answerer always yield
      // and an offerer with the lower PeerId yield too — both yielded, both bundles closed.
      if (ctx.local_wins_glare) {
        return CallMediaInboundHelloDecision::RejectGlare;
      }
      // Loser (or outbound not bound yet): accept inbound and drop any provisional outbound.
      return CallMediaInboundHelloDecision::AcceptAndYield;
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

CallMediaChannelCloseAction RouteCallMediaChannelClose(const CallMediaChannelCloseInput& in) {
  if (in.placeholder) {
    return CallMediaChannelCloseAction::DropPlaceholder;
  }
  switch (in.slot) {
    case CallMediaPathSlot::Candidate:
      return CallMediaChannelCloseAction::AbandonCandidate;
    case CallMediaPathSlot::Retiring:
      return CallMediaChannelCloseAction::DropRetiring;
    case CallMediaPathSlot::Standby:
      return CallMediaChannelCloseAction::DropStandby;
    case CallMediaPathSlot::Active:
      break;
  }
  // K014: whoever closed it — the peer's hang-up comes as `bye` first (the leg is gone by its close).
  if (in.phase == CallMediaBundlePhase::MediaReady && !in.local_cancel) {
    return CallMediaChannelCloseAction::ActivePathClosed;
  }
  if (in.phase == CallMediaBundlePhase::Idle) {
    return CallMediaChannelCloseAction::Ignore;
  }
  if (in.local_cancel || in.phase == CallMediaBundlePhase::Closing) {
    return CallMediaChannelCloseAction::TearDownQuietly;
  }
  // Dual-dial: the peer abandoned its outbound (our inbound) after we won glare, or we rejected it —
  // the winning outbound control stays.
  if (in.role == CallMediaChannelRole::InboundControl && in.phase == CallMediaBundlePhase::OutboundHello) {
    return CallMediaChannelCloseAction::DropInboundControl;
  }
  // Any other channel of a connecting leg closing fails it; the peer closing it is reported.
  return in.remote_terminal ? CallMediaChannelCloseAction::TearDownReported
                            : CallMediaChannelCloseAction::TearDownQuietly;
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

bool ShouldFailOverToStandby(const CallMediaFailoverInput& in) {
  if (!in.have_standby || !in.standby_link_alive) {
    return false;
  }
  if (in.peer_heartbeats && in.standby_silence_ms >= kCallMediaStandbyStaleMs) {
    return false;
  }
  if (in.active_link_lost) {
    return true;
  }
  return !in.awaiting_bye && in.peer_heartbeats && in.active_silence_ms >= kCallMediaActiveSilenceFailoverMs &&
         in.since_failover_ms >= kCallMediaFailoverHoldDownMs;
}

bool ShouldEnterPathLost(const CallMediaFailoverInput& in) {
  if (ShouldFailOverToStandby(in)) {
    return false;
  }
  if (in.active_link_lost) {
    return true;
  }
  return !in.awaiting_bye && in.peer_heartbeats && in.active_silence_ms >= kCallMediaActiveSilenceLostMs &&
         in.since_failover_ms >= kCallMediaFailoverHoldDownMs;
}

CallMediaActiveCloseAction DecideCallMediaActiveClose(const CallMediaActiveCloseInput& in) {
  if (in.role == CallMediaChannelRole::Media && in.remote_close && in.control_open) {
    return CallMediaActiveCloseAction::AwaitControl;
  }
  if (in.retiring_alive) {
    return CallMediaActiveCloseAction::FallBackToRetiring;
  }
  if (in.standby_alive) {
    return CallMediaActiveCloseAction::FailOverToStandby;
  }
  return CallMediaActiveCloseAction::PathLost;
}

bool CallMediaSeqWindow::Accept(const uint32_t seq) {
  const auto anchor = [&]() {
    have_ = true;
    highest_ = seq;
    seen_.reset();
    seen_.set(0);
    return true;
  };
  if (!have_) {
    return anchor();
  }
  if (seq > highest_) {
    const uint32_t shift = seq - highest_;
    if (shift >= kWindow) {
      seen_.reset();
    } else {
      seen_ <<= shift;
    }
    seen_.set(0);
    highest_ = seq;
    return true;
  }
  const uint32_t behind = highest_ - seq;
  if (behind >= kWindow) {
    return anchor(); // far behind: the sender restarted its seq
  }
  if (seen_.test(behind)) {
    return false;
  }
  seen_.set(behind);
  return true;
}

CallMediaRetiringAction DecideCallMediaRetiring(const CallMediaRetiringInput& in) {
  if (in.link_lost || in.since_switch_ms >= kCallMediaRetireAbandonMs) {
    return CallMediaRetiringAction::Drop;
  }
  if (in.drove_switch && !in.release_sent && in.can_send && in.since_switch_ms >= kCallMediaRetireAfterSwitchMs &&
      (in.rx_since_switch || in.since_switch_ms >= kCallMediaRetireAtMostMs)) {
    return CallMediaRetiringAction::SendRelease;
  }
  return CallMediaRetiringAction::Keep;
}

} // namespace pbr
