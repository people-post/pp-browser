#include "domain/messaging/CallLifecycleTransitionLogic.h"

namespace pbr {

CallLifecycleTransitionOutcome DecideCallLifecycleTransition(CallLifecycleEvent ev,
                                                             const CallLifecycleTransitionContext& ctx) {
  CallLifecycleTransitionOutcome out;
  out.call_id = ctx.event_call_id;
  if (out.call_id.empty()) {
    out.call_id = ctx.active_call_id;
  }
  out.next_phase = ctx.phase;
  out.next_status = ctx.status;

  auto ignore = [&](const char* reason) {
    out.actions = CallLifecycleAction::LogIgnored;
    out.ignore_reason = reason;
    return out;
  };

  switch (ev) {
  case CallLifecycleEvent::InviteSeen:
    if (ctx.phase == CallPhase::Idle || ctx.phase == CallPhase::Ringing) {
      out.actions = CallLifecycleAction::NoteRing | CallLifecycleAction::SetPhase |
                    CallLifecycleAction::NotifyChrome;
      out.next_phase = CallPhase::Ringing;
      return out;
    }
    if (ctx.phase == CallPhase::OutboundCalling || ctx.phase == CallPhase::InCall ||
        ctx.phase == CallPhase::MediaConnecting || ctx.phase == CallPhase::JoinedLocal ||
        ctx.phase == CallPhase::MediaPending || ctx.phase == CallPhase::ConnectFailed) {
      out.actions = CallLifecycleAction::NoteRing | CallLifecycleAction::NotifyChrome;
      return out;
    }
    return out;

  case CallLifecycleEvent::InviteCleared:
    if (ctx.phase == CallPhase::Ringing) {
      out.actions = CallLifecycleAction::SetPhase | CallLifecycleAction::NotifyChrome;
      out.next_phase = CallPhase::Idle;
      out.call_id.clear();
    }
    return out;

  case CallLifecycleEvent::OutboundStarted:
    out.actions = CallLifecycleAction::SetPhase | CallLifecycleAction::SetStatus |
                  CallLifecycleAction::NotifyChrome;
    out.next_phase = CallPhase::OutboundCalling;
    out.next_status = CallMediaStatus::Deciding;
    out.status_reason = "OutboundStarted";
    return out;

  case CallLifecycleEvent::AcceptClicked: {
    if (out.call_id.empty()) {
      out.call_id = ctx.last_ring_call_id;
    }
    if (out.call_id.empty()) {
      return ignore("AcceptClicked ignored (no call_id)");
    }
    if (!ctx.accepting_call_id.empty() && ctx.accepting_call_id == out.call_id) {
      out.actions = CallLifecycleAction::NotifyChrome | CallLifecycleAction::LogIgnored;
      out.ignore_reason = "AcceptClicked already in flight";
      return out;
    }
    out.actions = CallLifecycleAction::NoteRing | CallLifecycleAction::SetAccepting |
                  CallLifecycleAction::SetPhase | CallLifecycleAction::PostAcceptInvite |
                  CallLifecycleAction::DeferChrome;
    out.next_phase = CallPhase::Accepting;
    return out;
  }

  case CallLifecycleEvent::DeclineClicked: {
    if (out.call_id.empty()) {
      out.call_id = ctx.last_ring_call_id;
    }
    if (out.call_id.empty()) {
      return ignore("DeclineClicked ignored (no call_id)");
    }
    out.actions = CallLifecycleAction::SetPhase | CallLifecycleAction::NotifyChrome |
                  CallLifecycleAction::PostDeclineInvite;
    out.next_phase = CallPhase::Idle;
    return out;
  }

  case CallLifecycleEvent::LeaveClicked:
    if (out.call_id.empty()) {
      return ignore("LeaveClicked ignored (no call_id)");
    }
    out.actions = CallLifecycleAction::SetPhase | CallLifecycleAction::NotifyChrome |
                  CallLifecycleAction::PostLeaveCall;
    out.next_phase = CallPhase::Idle;
    return out;

  case CallLifecycleEvent::RetryClicked:
    if (out.call_id.empty() || ctx.phase != CallPhase::ConnectFailed) {
      return ignore("RetryClicked ignored");
    }
    out.actions = CallLifecycleAction::PostRetryMedia;
    return out;

  case CallLifecycleEvent::AcceptSucceeded:
    if (!out.call_id.empty() && !ctx.active_call_id.empty() && out.call_id != ctx.active_call_id) {
      return ignore("AcceptSucceeded ignored stale call_id");
    }
    out.actions = CallLifecycleAction::ClearAccepting | CallLifecycleAction::NotifyChrome;
    if (ctx.phase != CallPhase::MediaPending && ctx.phase != CallPhase::MediaConnecting &&
        ctx.phase != CallPhase::InCall) {
      out.actions |= CallLifecycleAction::SetPhase;
      out.next_phase = CallPhase::JoinedLocal;
    } else {
      out.actions |= CallLifecycleAction::LogKeepPhase;
    }
    if (ctx.sessions_bound && ctx.allows_direct_path) {
      out.actions |= CallLifecycleAction::KickAnswererDirectMedia;
      if (out.call_id.empty()) {
        out.call_id = ctx.active_call_id;
      }
    }
    return out;

  case CallLifecycleEvent::AcceptFailed:
    if (!out.call_id.empty() && !ctx.active_call_id.empty() && out.call_id != ctx.active_call_id) {
      return ignore("AcceptFailed ignored stale call_id");
    }
    out.actions = CallLifecycleAction::ClearAccepting | CallLifecycleAction::SetPhase |
                  CallLifecycleAction::NotifyChrome;
    out.next_phase = CallPhase::Ringing;
    return out;

  case CallLifecycleEvent::DeclineDone:
  case CallLifecycleEvent::LeaveDone:
    out.actions = CallLifecycleAction::ClearAccepting | CallLifecycleAction::SetPhase |
                  CallLifecycleAction::NotifyChrome;
    out.next_phase = CallPhase::Idle;
    out.call_id.clear();
    return out;

  case CallLifecycleEvent::RemoteEnded:
    if (!out.call_id.empty() && !ctx.active_call_id.empty() && out.call_id != ctx.active_call_id) {
      return ignore("RemoteEnded ignored stale call_id");
    }
    out.actions = CallLifecycleAction::ClearAccepting | CallLifecycleAction::SetPhase |
                  CallLifecycleAction::NotifyChrome;
    out.next_phase = CallPhase::Idle;
    out.call_id.clear();
    return out;

  case CallLifecycleEvent::MediaDeferred:
    if (ctx.phase == CallPhase::Accepting || ctx.phase == CallPhase::JoinedLocal ||
        ctx.phase == CallPhase::OutboundCalling || ctx.phase == CallPhase::MediaConnecting) {
      out.actions = CallLifecycleAction::SetPhase | CallLifecycleAction::NotifyChrome;
      out.next_phase = CallPhase::MediaPending;
    }
    return out;

  case CallLifecycleEvent::MediaKeyReady:
    if (ctx.phase == CallPhase::MediaPending || ctx.phase == CallPhase::JoinedLocal ||
        ctx.phase == CallPhase::Accepting) {
      out.actions = CallLifecycleAction::SetPhase | CallLifecycleAction::NotifyChrome;
      out.next_phase = CallPhase::MediaConnecting;
    }
    return out;

  case CallLifecycleEvent::DirectConnected:
    out.actions = CallLifecycleAction::SetPhase | CallLifecycleAction::NotifyChrome;
    out.next_phase = CallPhase::InCall;
    out.status_reason = "DirectConnected";
    if (ctx.status == CallMediaStatus::HopAttaching || ctx.status == CallMediaStatus::HopWaiting ||
        ctx.status == CallMediaStatus::HopLive || ctx.status == CallMediaStatus::Migrating) {
      out.actions |= CallLifecycleAction::SetStatus;
      out.next_status = CallMediaStatus::HopLive;
    } else if (ctx.status != CallMediaStatus::DirectLive && ctx.status != CallMediaStatus::HopLive) {
      out.actions |= CallLifecycleAction::SetStatus;
      out.next_status = CallMediaStatus::DirectLive;
    }
    return out;

  case CallLifecycleEvent::ConnectFailedEvt:
    if (ctx.phase != CallPhase::Idle) {
      out.actions = CallLifecycleAction::SetPhase | CallLifecycleAction::NotifyChrome;
      out.next_phase = CallPhase::ConnectFailed;
    }
    return out;
  }

  return out;
}

} // namespace pbr
