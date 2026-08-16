#include "base/p2p/CallMediaSessionLogic.h"

namespace pbr {

CallMediaSessionPhaseOutcome DecideCallMediaSessionPhase(CallMediaSessionPhase phase,
                                                         CallMediaSessionEvent ev,
                                                         const CallMediaSessionApplyContext& ctx) {
  CallMediaSessionPhaseOutcome out;
  switch (ev) {
  case CallMediaSessionEvent::ConnectRequested:
    out.decision = CallMediaSessionPhaseDecision::Transition;
    out.next = CallMediaSessionPhase::Dialing;
    return out;

  case CallMediaSessionEvent::OpenStreamOk:
    if (phase == CallMediaSessionPhase::Dialing) {
      out.decision = CallMediaSessionPhaseDecision::Transition;
      out.next = CallMediaSessionPhase::HelloOutbound;
      return out;
    }
    // Inbound hello failed while Connect waiter still live — resume outbound.
    if (phase == CallMediaSessionPhase::Idle && ctx.connect_waiter_active) {
      out.decision = CallMediaSessionPhaseDecision::Transition;
      out.next = CallMediaSessionPhase::Dialing;
      return out;
    }
    // Dual-dial: outbound hello proceeds without stealing HelloInbound.
    if (phase == CallMediaSessionPhase::HelloInbound) {
      out.decision = CallMediaSessionPhaseDecision::Keep;
      out.next = phase;
      return out;
    }
    return out; // Ignore (Detaching / Idle without waiter / unexpected)

  case CallMediaSessionEvent::OpenStreamFail:
    if (phase == CallMediaSessionPhase::Dialing || phase == CallMediaSessionPhase::HelloOutbound) {
      out.decision = CallMediaSessionPhaseDecision::Transition;
      out.next = CallMediaSessionPhase::Idle;
      return out;
    }
    return out;

  case CallMediaSessionEvent::InboundStream:
    out.decision = CallMediaSessionPhaseDecision::Transition;
    out.next = CallMediaSessionPhase::HelloInbound;
    return out;

  case CallMediaSessionEvent::HelloOk:
    if (phase == CallMediaSessionPhase::HelloInbound) {
      out.decision = CallMediaSessionPhaseDecision::Transition;
      out.next = CallMediaSessionPhase::HelloInbound;
      return out;
    }
    if (phase == CallMediaSessionPhase::HelloOutbound || phase == CallMediaSessionPhase::Dialing) {
      out.decision = CallMediaSessionPhaseDecision::Transition;
      out.next = CallMediaSessionPhase::HelloOutbound;
      return out;
    }
    return out;

  case CallMediaSessionEvent::HelloFail:
    if (phase == CallMediaSessionPhase::HelloInbound) {
      out.decision = CallMediaSessionPhaseDecision::Transition;
      out.next = (ctx.connect_waiter_active && !ctx.has_stream) ? CallMediaSessionPhase::Dialing
                                                                : CallMediaSessionPhase::Idle;
      return out;
    }
    if (phase == CallMediaSessionPhase::HelloOutbound || phase == CallMediaSessionPhase::Dialing) {
      out.decision = CallMediaSessionPhaseDecision::Transition;
      out.next = CallMediaSessionPhase::Idle;
      return out;
    }
    return out;

  case CallMediaSessionEvent::AdoptWon:
    out.decision = CallMediaSessionPhaseDecision::Transition;
    out.next = CallMediaSessionPhase::Adopting;
    return out;

  case CallMediaSessionEvent::AdoptLost:
    if (phase == CallMediaSessionPhase::HelloInbound) {
      out.decision = CallMediaSessionPhaseDecision::Transition;
      out.next = (ctx.connect_waiter_active && !ctx.has_stream) ? CallMediaSessionPhase::Dialing
                                                                : CallMediaSessionPhase::Idle;
      return out;
    }
    if (phase == CallMediaSessionPhase::HelloOutbound || phase == CallMediaSessionPhase::Dialing) {
      out.decision = CallMediaSessionPhaseDecision::Transition;
      out.next = CallMediaSessionPhase::Idle;
      return out;
    }
    return out;

  case CallMediaSessionEvent::DuplexStarted:
    if (phase == CallMediaSessionPhase::Adopting || phase == CallMediaSessionPhase::HelloOutbound ||
        phase == CallMediaSessionPhase::HelloInbound) {
      out.decision = CallMediaSessionPhaseDecision::Transition;
      out.next = CallMediaSessionPhase::MediaReady;
      return out;
    }
    if (phase == CallMediaSessionPhase::MediaReady) {
      out.decision = CallMediaSessionPhaseDecision::Keep;
      out.next = phase;
      return out;
    }
    return out;

  case CallMediaSessionEvent::ConnectTimeout:
    if (phase == CallMediaSessionPhase::Dialing || phase == CallMediaSessionPhase::HelloOutbound) {
      out.decision = CallMediaSessionPhaseDecision::Transition;
      out.next = CallMediaSessionPhase::Idle;
      return out;
    }
    return out;

  case CallMediaSessionEvent::DetachRequested:
    // Late outbound abort (settled mid-hello) — Idle without full DetachLocked teardown.
    // MediaReady / Adopting teardown uses DetachLocked → SetPhase(Detaching→Idle).
    if (phase == CallMediaSessionPhase::Dialing || phase == CallMediaSessionPhase::HelloOutbound) {
      out.decision = CallMediaSessionPhaseDecision::Transition;
      out.next = CallMediaSessionPhase::Idle;
      return out;
    }
    out.decision = CallMediaSessionPhaseDecision::Keep;
    out.next = phase;
    return out;

  case CallMediaSessionEvent::DuplexEof:
  case CallMediaSessionEvent::DuplexError:
  case CallMediaSessionEvent::ConnectSuperseded:
  case CallMediaSessionEvent::HandlerCleared:
    // Compound effects live in Fail / DetachLocked / ClearInboundHandler.
    out.decision = CallMediaSessionPhaseDecision::Keep;
    out.next = phase;
    return out;
  }
  return out;
}

bool CallMediaFailNotifySuppressed(CallMediaSessionPhase phase) {
  return phase == CallMediaSessionPhase::Idle || phase == CallMediaSessionPhase::Detaching;
}

bool LocalWinsCallMediaGlare(const std::string& local_peer_id, const std::string& remote_peer_id) {
  if (local_peer_id.empty() || remote_peer_id.empty()) {
    return true;
  }
  return local_peer_id > remote_peer_id;
}

} // namespace pbr
