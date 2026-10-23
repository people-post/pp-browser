// The group path's decisions as people join a call (CallTopologyController runs the outcomes).
#include "lib/calls/CallHopJoinLogic.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(CallHopJoinLogicTest, TheGroupPathServesOneCallAtATime) {
  HopActiveCallInput in;
  EXPECT_FALSE(IsHopActiveCall(in)) << "no call id";
  in.call_id = "call:a";
  in.call_session_active = true;
  EXPECT_TRUE(IsHopActiveCall(in)) << "its session is active and nothing else holds the path";

  in.flight_call_id = "call:b";
  EXPECT_FALSE(IsHopActiveCall(in)) << "a migrate in flight for another call is exclusive";
  in.flight_call_id = "call:a";
  in.call_session_active = false;
  EXPECT_TRUE(IsHopActiveCall(in)) << "and so is one for this call, whatever the store says";
  in.flight_call_id.clear();

  in.attach_wait_call_id = "call:b";
  EXPECT_FALSE(IsHopActiveCall(in));
  in.attach_wait_call_id.clear();

  in.call_session_active = true;
  in.seat_bound = true;
  in.seat_bound_call_id = "call:b";
  EXPECT_FALSE(IsHopActiveCall(in)) << "the seat holds another call: a zombie Active row is not ours";
  in.seat_bound_call_id.clear();
  EXPECT_TRUE(IsHopActiveCall(in)) << "a bound seat with no call leaves it to the store";
  in.seat_bound_to_call = true;
  in.call_session_active = false;
  EXPECT_TRUE(IsHopActiveCall(in)) << "the seat's bind is the media-active answer";

  HopActiveCallInput guest;
  guest.call_id = "call:a";
  guest.call_session_active = true;
  guest.guest_call_id = "call:b";
  guest.guest_session_active = true;
  EXPECT_FALSE(IsHopActiveCall(guest)) << "no seat: another call's live guest attach is exclusive";
  guest.guest_session_active = false;
  EXPECT_TRUE(IsHopActiveCall(guest)) << "unless that call ended";
}

TEST(CallHopJoinLogicTest, OurAcceptStaysDirectUntilThreeJoined) {
  EXPECT_EQ(DecideLocalAcceptHop(2, true), LocalAcceptHopAction::StayDirect) << "a hint never makes 1:1 a group";
  EXPECT_EQ(DecideLocalAcceptHop(3, true), LocalAcceptHopAction::AttachToInviteHint);
  EXPECT_EQ(DecideLocalAcceptHop(3, false), LocalAcceptHopAction::JoinGroup);
  // V053: a group call on its hop, joined back at two (someone was waiting alone): join it there.
  EXPECT_EQ(DecideLocalAcceptHop(2, true, /*group_call=*/true), LocalAcceptHopAction::AttachToInviteHint);
  EXPECT_EQ(DecideLocalAcceptHop(2, false, /*group_call=*/true), LocalAcceptHopAction::StayDirect);

  JoinWithoutHintInput in;
  EXPECT_EQ(DecideJoinWithoutHint(in), JoinWithoutHintAction::WaitForAttach) << "phones wait for the owner";
  in.may_prefer_local_hop = true;
  EXPECT_EQ(DecideJoinWithoutHint(in), JoinWithoutHintAction::PickHop);
  in.flight_busy_for_call = true;
  EXPECT_EQ(DecideJoinWithoutHint(in), JoinWithoutHintAction::KeepFlight) << "an inbound attach raced the accept";
  in.on_hop_for_call = true;
  EXPECT_EQ(DecideJoinWithoutHint(in), JoinWithoutHintAction::AlreadyOnHop);
}

TEST(CallHopJoinLogicTest, APeersAcceptOfOurCall) {
  RemoteAcceptHopInput in;
  in.n_joined = 3;
  EXPECT_EQ(DecideRemoteAcceptHop(in), RemoteAcceptHopAction::IgnoreNotActive) << "a stale call";
  in.active_call = true;
  in.n_joined = 2;
  EXPECT_EQ(DecideRemoteAcceptHop(in), RemoteAcceptHopAction::StayDirect);
  in.n_joined = 3;
  EXPECT_EQ(DecideRemoteAcceptHop(in), RemoteAcceptHopAction::ResolveAndMigrate);
  in.migrate_in_flight = true;
  EXPECT_EQ(DecideRemoteAcceptHop(in), RemoteAcceptHopAction::JoinFlight) << "no second resolve or migrate";
  in.on_hop_for_call = true;
  EXPECT_EQ(DecideRemoteAcceptHop(in), RemoteAcceptHopAction::RefreshOnHop);
  in.migrate_in_flight = false;
  in.n_joined = 2;
  EXPECT_EQ(DecideRemoteAcceptHop(in), RemoteAcceptHopAction::RefreshOnHop)
      << "V053: on the hop, a call stays there when someone rejoins one waiting alone";
}

TEST(CallHopJoinLogicTest, TheRosterMigratesOnlyAFirstGroup) {
  JoinedCountHopInput in;
  in.n_joined = 2;
  EXPECT_EQ(DecideJoinedCountHop(in), JoinedCountHopAction::None);
  in.n_joined = 3;
  EXPECT_EQ(DecideJoinedCountHop(in), JoinedCountHopAction::Migrate);
  in.planned_hop = true;
  EXPECT_EQ(DecideJoinedCountHop(in), JoinedCountHopAction::Migrate) << "another member's joiner: its roster is all we see";
  in.initiator_awaits_own_invitee = true;
  EXPECT_EQ(DecideJoinedCountHop(in), JoinedCountHopAction::AwaitJoinerAccept) << "its accept carries the hop report";
  in.has_sfu_hint = true;
  EXPECT_EQ(DecideJoinedCountHop(in), JoinedCountHopAction::None) << "the call already has its hop";
  in.attach_wait_for_call = true;
  EXPECT_EQ(DecideJoinedCountHop(in), JoinedCountHopAction::SkipAttachWait);
  in.migrate_in_flight = true;
  EXPECT_EQ(DecideJoinedCountHop(in), JoinedCountHopAction::SkipInFlight);
  in.on_hop_for_call = true;
  EXPECT_EQ(DecideJoinedCountHop(in), JoinedCountHopAction::SyncSubscriptions);
}

TEST(CallHopJoinLogicTest, AStrayAttachNeverTakesA1To1Call) {
  InboundAttachInput in;
  in.active_call = true;
  in.status_allows_hop = true;
  in.n_joined = 2;
  EXPECT_FALSE(ExpectsGroupAttach(in));
  in.attach_wait_for_call = true;
  EXPECT_TRUE(ExpectsGroupAttach(in)) << "we wait for it";
  in.status_allows_hop = false;
  EXPECT_FALSE(ExpectsGroupAttach(in)) << "the call's Status arms the direct path";
  in.status_allows_hop = true;
  in.attach_wait_for_call = false;
  in.n_joined = 3;
  EXPECT_TRUE(ExpectsGroupAttach(in));
  in.active_call = false;
  EXPECT_FALSE(ExpectsGroupAttach(in));
}

TEST(CallHopJoinLogicTest, AJoinersHopFailure) {
  HopFailureInput in;
  in.guest = "account:g";
  in.owner = {HopHintOwnerAction::RePick, "hop:x"};
  EXPECT_EQ(DecideHopFailure(in), HopFailureAction::Ignore) << "only the owner answers";
  in.sticky_initiator = true;
  EXPECT_EQ(DecideHopFailure(in), HopFailureAction::Repick);
  in.migrate_in_flight = true;
  EXPECT_EQ(DecideHopFailure(in), HopFailureAction::Coalesce);
  in.on_pick = true;
  EXPECT_EQ(DecideHopFailure(in), HopFailureAction::RefanOut) << "already there: no second detach";
  in.may_leave_prefer_local = false;
  EXPECT_EQ(DecideHopFailure(in), HopFailureAction::RefuseGuest);
  in.may_leave_prefer_local = true;
  in.owner = {HopHintOwnerAction::RefuseGuest, ""};
  EXPECT_EQ(DecideHopFailure(in), HopFailureAction::RefuseGuest) << "no hop serves the joiner and the group";
  in.owner = {HopHintOwnerAction::RePick, "hop:x"};
  in.already_moved_guest_to_failed_hop = true;
  EXPECT_EQ(DecideHopFailure(in), HopFailureAction::RefuseGuest) << "one hop change per joiner";
  in.guest.clear();
  EXPECT_EQ(DecideHopFailure(in), HopFailureAction::Ignore) << "nobody to refuse";
}

} // namespace
} // namespace pbr
