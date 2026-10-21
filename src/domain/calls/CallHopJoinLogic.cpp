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

LocalAcceptHopAction DecideLocalAcceptHop(const size_t n_joined, const bool has_invite_hint) {
  if (!ShouldArmHopPlanner(n_joined)) {
    return LocalAcceptHopAction::StayDirect;
  }
  return has_invite_hint ? LocalAcceptHopAction::AttachToInviteHint : LocalAcceptHopAction::JoinGroup;
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
  if (!ShouldArmHopPlanner(in.n_joined)) {
    return RemoteAcceptHopAction::StayDirect;
  }
  if (in.on_hop_for_call) {
    return RemoteAcceptHopAction::RefreshOnHop;
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
