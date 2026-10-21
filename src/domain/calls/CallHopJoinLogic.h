#pragma once

#include "domain/calls/CallHopAttachLogic.h"

#include <cstddef>
#include <string>

namespace pbr {

/**
 * Pure group-path decisions as people join a call: our own accept, a peer's accept of our call,
 * the roster's joined count, an inbound CallSfuAttach and a joiner's hop failure.
 * CallTopologyController gathers the facts (flight, attach wait, seat, store) and runs the outcome.
 */

// --- Which call the group path serves ---

struct HopActiveCallInput {
  std::string call_id;
  /** The call a SoftMigrate / attach is in flight for (empty: none). */
  std::string flight_call_id;
  /** The call waiting for the owner's CallSfuAttach (empty: none). */
  std::string attach_wait_call_id;
  /** A seat is bound (V036: its bind is the media-active answer). */
  bool seat_bound = false;
  /** The seat is bound to this call. */
  bool seat_bound_to_call = false;
  /** The call the seat is bound to (empty: bound to none). */
  std::string seat_bound_call_id;
  /** No seat: the call a guest attach bound (unit tests), and whether its session is still active. */
  std::string guest_call_id;
  bool guest_session_active = false;
  /** This call's stored session is active. */
  bool call_session_active = false;
};

/** Whether the group path may act for `call_id` (one call at a time; a zombie Active row never). */
bool IsHopActiveCall(const HopActiveCallInput& in);

// --- Our accept landed ---

enum class LocalAcceptHopAction {
  /** A group with the inviter's hop hint: attach straight to it. */
  AttachToInviteHint,
  /** A group without a hint: wait for the owner's attach, or pick a hop when this device may. */
  JoinGroup,
  /** 1:1: the direct path takes the call. */
  StayDirect,
};

LocalAcceptHopAction DecideLocalAcceptHop(size_t n_joined, bool has_invite_hint);

struct JoinWithoutHintInput {
  /** Media already runs on a hop for this call. */
  bool on_hop_for_call = false;
  /** A SoftMigrate / attach for this call is in flight (an inbound attach raced our accept). */
  bool flight_busy_for_call = false;
  /** This device may host the hop itself (PreferLocal with its relay started). */
  bool may_prefer_local_hop = false;
};

enum class JoinWithoutHintAction {
  /** Already on the hop: subscribe and report live. */
  AlreadyOnHop,
  /** Keep the in-flight attach (re-claiming would abort it before it starts). */
  KeepFlight,
  /** Wait for the owner's CallSfuAttach. */
  WaitForAttach,
  /** Pick a hop (SoftMigrate). */
  PickHop,
};

JoinWithoutHintAction DecideJoinWithoutHint(const JoinWithoutHintInput& in);

// --- A peer accepted our call ---

struct RemoteAcceptHopInput {
  bool active_call = false;
  size_t n_joined = 0;
  bool on_hop_for_call = false;
  bool migrate_in_flight = false;
};

enum class RemoteAcceptHopAction {
  /** Not the call the group path serves: nothing (and no direct start for a stale call). */
  IgnoreNotActive,
  /** 1:1: the direct path takes it. */
  StayDirect,
  /** Already on the hop: subscribe and re-fan-out the attach for the late joiner. */
  RefreshOnHop,
  /** A migrate is in flight: the joiner rides it. */
  JoinFlight,
  /** V050: resolve the hop for the joiner (keep / one adjustment / refuse), then migrate. */
  ResolveAndMigrate,
};

RemoteAcceptHopAction DecideRemoteAcceptHop(const RemoteAcceptHopInput& in);

// --- The roster shows who joined ---

struct JoinedCountHopInput {
  size_t n_joined = 0;
  bool on_hop_for_call = false;
  bool migrate_in_flight = false;
  bool attach_wait_for_call = false;
  /** The call already has a hop (its first attach happened). */
  bool has_sfu_hint = false;
  bool planned_hop = false;
  /** We placed the call and one of our own invitees has not accepted yet. */
  bool initiator_awaits_own_invitee = false;
};

enum class JoinedCountHopAction {
  /** 1:1, or the call already has its hop. */
  None,
  /** On the hop already: subscribe to late joiners. */
  SyncSubscriptions,
  /** A migrate is in flight: it covers the new joiner. */
  SkipInFlight,
  /** Waiting for the owner's attach: ask the inbox for it instead of migrating again. */
  SkipAttachWait,
  /** V050: a planned hop forms from the joiner's CallAccept (it carries the hop report). */
  AwaitJoinerAccept,
  /** The call became a group: migrate to a hop. */
  Migrate,
};

JoinedCountHopAction DecideJoinedCountHop(const JoinedCountHopInput& in);

// --- An owner's CallSfuAttach arrives ---

struct InboundAttachInput {
  bool active_call = false;
  /** No arming bound, or the call's Status lets the hop act (V037). */
  bool status_allows_hop = false;
  size_t n_joined = 0;
  bool attach_wait_for_call = false;
  bool flight_for_call = false;
  bool on_hop = false;
};

/** 1:1 stays on its direct stream: a stray attach (inflated roster) would race the direct start. */
bool ExpectsGroupAttach(const InboundAttachInput& in);

// --- A joiner could not reach the group's hop (we own the call) ---

struct HopFailureInput {
  bool sticky_initiator = false;
  std::string guest;
  /** The group already moved to the failed hop for this joiner (one change per joiner). */
  bool already_moved_guest_to_failed_hop = false;
  /** The owner's pick among hops the joiner and the other members reached. */
  HopHintOwnerDecision owner;
  /** Leaving our own PreferLocal hop for the pick is allowed. */
  bool may_leave_prefer_local = true;
  /** Media already runs on (or is hinted to) the pick. */
  bool on_pick = false;
  bool migrate_in_flight = false;
};

enum class HopFailureAction {
  /** Not ours to answer. */
  Ignore,
  /** No hop serves the joiner and the group: refuse the joiner. */
  RefuseGuest,
  /** Already on the pick: re-fan-out its attach. */
  RefanOut,
  /** A migrate is in flight: it takes the pick when done. */
  Coalesce,
  /** Move the group to the pick. */
  Repick,
};

HopFailureAction DecideHopFailure(const HopFailureInput& in);

} // namespace pbr
