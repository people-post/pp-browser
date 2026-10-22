#include "domain/calls/CallHopJoinLogic.h"

#include "domain/calls/CallMediaPlannerSelectLogic.h"

#include "common/PbrCompat.h"

namespace pbr {

bool IsHopActiveCall(const HopActiveCallInput& in) {
  if (in.call_id.empty()) {
    return false;
  }
  // SoftMigrate / WaitForAttach are exclusive while in flight (zombie Active disk rows).
  if (!in.flight_call_id.empty()) {
    return in.flight_call_id == in.call_id;
  }
  if (!in.attach_wait_call_id.empty()) {
    return in.attach_wait_call_id == in.call_id;
  }
  // V036: the seat's bind is the media-active answer — never vetoed by a leftover engine call id.
  if (in.seat_bound) {
    if (in.seat_bound_to_call) {
      return true;
    }
    if (!in.seat_bound_call_id.empty()) {
      return false;
    }
  } else if (!in.guest_call_id.empty()) {
    // A guest attach bound without a seat (unit tests): exclusive while its call is still active.
    if (in.guest_call_id == in.call_id) {
      return true;
    }
    if (in.guest_session_active) {
      return false;
    }
  }
  return in.call_session_active;
}

LocalAcceptHopAction DecideLocalAcceptHop(const size_t n_joined, const bool has_invite_hint, const bool group_call) {
  // A group call's invite naming the hop it is on: the call went there and never comes back
  // (V050) — whoever joins it now joins it there, however few are in it (V053: someone waiting
  // alone). A 1:1's hint is stale: a hint never makes 1:1 a group (V025 / V038).
  if (has_invite_hint && (group_call || ShouldArmHopPlanner(n_joined))) {
    return LocalAcceptHopAction::AttachToInviteHint;
  }
  return ShouldArmHopPlanner(n_joined) ? LocalAcceptHopAction::JoinGroup : LocalAcceptHopAction::StayDirect;
}

JoinWithoutHintAction DecideJoinWithoutHint(const JoinWithoutHintInput& in) {
  if (in.on_hop_for_call) {
    return JoinWithoutHintAction::AlreadyOnHop;
  }
  if (in.flight_busy_for_call) {
    return JoinWithoutHintAction::KeepFlight;
  }
  return in.may_prefer_local_hop ? JoinWithoutHintAction::PickHop : JoinWithoutHintAction::WaitForAttach;
}

RemoteAcceptHopAction DecideRemoteAcceptHop(const RemoteAcceptHopInput& in) {
  if (!in.active_call) {
    return RemoteAcceptHopAction::IgnoreNotActive;
  }
  // On the hop already: the call stays there whoever joins (V050 monotonic; V053 a rejoin at 2).
  if (in.on_hop_for_call) {
    return RemoteAcceptHopAction::RefreshOnHop;
  }
  if (!ShouldArmHopPlanner(in.n_joined)) {
    return RemoteAcceptHopAction::StayDirect;
  }
  return in.migrate_in_flight ? RemoteAcceptHopAction::JoinFlight : RemoteAcceptHopAction::ResolveAndMigrate;
}

JoinedCountHopAction DecideJoinedCountHop(const JoinedCountHopInput& in) {
  if (!ShouldArmHopPlanner(in.n_joined)) {
    return JoinedCountHopAction::None;
  }
  if (in.on_hop_for_call) {
    return JoinedCountHopAction::SyncSubscriptions;
  }
  if (in.migrate_in_flight) {
    return JoinedCountHopAction::SkipInFlight;
  }
  if (in.attach_wait_for_call) {
    return JoinedCountHopAction::SkipAttachWait;
  }
  if (in.has_sfu_hint) {
    return JoinedCountHopAction::None;
  }
  // V050: with a planned hop the initiator forms the group from the joiner's CallAccept — a roster
  // showing the join first (transports reorder) must not migrate without its hop report. Only a
  // joiner this device invited accepts here; one another member added accepts to that member.
  if (in.planned_hop && in.initiator_awaits_own_invitee) {
    return JoinedCountHopAction::AwaitJoinerAccept;
  }
  return JoinedCountHopAction::Migrate;
}

bool ExpectsGroupAttach(const InboundAttachInput& in) {
  if (!in.active_call || !in.status_allows_hop) {
    return false;
  }
  return ShouldArmHopPlanner(in.n_joined) || in.attach_wait_for_call || in.flight_for_call || in.on_hop;
}

HopFailureAction DecideHopFailure(const HopFailureInput& in) {
  if (!in.sticky_initiator) {
    return HopFailureAction::Ignore;  // only the sticky initiator answers hop hints
  }
  if (!in.guest.empty() && in.already_moved_guest_to_failed_hop) {
    return HopFailureAction::RefuseGuest;  // V050: one hop change per joiner
  }
  if (in.owner.action == HopHintOwnerAction::RefuseGuest || in.guest.empty()) {
    return in.guest.empty() ? HopFailureAction::Ignore : HopFailureAction::RefuseGuest;
  }
  if (!in.may_leave_prefer_local) {
    return HopFailureAction::RefuseGuest;
  }
  if (in.on_pick && !in.owner.preferred_hop_peer_id.empty()) {
    return HopFailureAction::RefanOut;
  }
  return in.migrate_in_flight ? HopFailureAction::Coalesce : HopFailureAction::Repick;
}

} // namespace pbr
