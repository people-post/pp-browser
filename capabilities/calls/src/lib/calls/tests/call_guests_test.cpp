#include "lib/calls/CallControlCodec.h"
#include "lib/calls/CallControlGate.h"
#include "lib/calls/CallGuests.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

// V062 / group-chat G017: what each role may do in a group's call under each preset (the product layer maps roles).
CallGroupStanding Owner() {
  return {.member = true, .may_bring_guest = true, .may_remove_guest = true, .may_invite_to_group = true};
}
CallGroupStanding Admin() {
  return {.member = true, .may_bring_guest = true, .may_remove_guest = true};
}
CallGroupStanding OpenMember() {
  return {.member = true, .may_bring_guest = true};
}
CallGroupStanding ManagedMember() {
  return {.member = true};
}
CallGroupStanding Stranger() {
  return {};
}
constexpr std::optional<CallGroupStanding> kNoRoster = std::nullopt;

// V062 6: a call with no group is unchanged — anyone adds anyone, no guest label.
TEST(CallGuestsTest, CallWithNoGroupAddsAnyoneAsAnOrdinaryInvite) {
  EXPECT_EQ(DecideCallAdd(false, kNoRoster, kNoRoster), CallAddVerdict::Plain);
  EXPECT_EQ(DecideCallAdd(false, ManagedMember(), Stranger()), CallAddVerdict::Plain);
  EXPECT_FALSE(ShowsAsGuest(false, "account:bob", Stranger()));
}

// V062 2: a member allowed to invite under the preset brings a guest; inviting a member is no guest invite.
TEST(CallGuestsTest, MembersAllowedToInviteBringGuests) {
  EXPECT_EQ(DecideCallAdd(true, OpenMember(), Stranger()), CallAddVerdict::Guest);
  EXPECT_EQ(DecideCallAdd(true, Admin(), Stranger()), CallAddVerdict::Guest);
  EXPECT_EQ(DecideCallAdd(true, Owner(), Stranger()), CallAddVerdict::Guest);
  EXPECT_EQ(DecideCallAdd(true, ManagedMember(), OpenMember()), CallAddVerdict::Plain);
}

// V062 2: a Managed group's plain member may not bring a guest; a guest's device (no roster) adds nobody.
TEST(CallGuestsTest, ManagedMembersAndGuestsBringNoGuests) {
  EXPECT_EQ(DecideCallAdd(true, ManagedMember(), Stranger()), CallAddVerdict::Refused);
  EXPECT_EQ(DecideCallAdd(true, kNoRoster, kNoRoster), CallAddVerdict::Refused);
  EXPECT_EQ(DecideCallAdd(true, Stranger(), Stranger()), CallAddVerdict::Refused);
}

// V062 2: receivers take a guest's roster entry only from a bringer allowed to bring one; members always.
TEST(CallGuestsTest, ReceiversDropAGuestFromABringerNotAllowed) {
  EXPECT_TRUE(TakesCallRosterEntry(OpenMember(), "", kNoRoster));
  EXPECT_TRUE(TakesCallRosterEntry(Stranger(), "account:bob", OpenMember()));
  EXPECT_FALSE(TakesCallRosterEntry(Stranger(), "account:bob", ManagedMember()));
  EXPECT_FALSE(TakesCallRosterEntry(Stranger(), "account:erin", Stranger())) << "guests do not bring guests";
  EXPECT_FALSE(TakesCallRosterEntry(Stranger(), "", kNoRoster)) << "a non-member nobody brought";
  EXPECT_TRUE(TakesCallRosterEntry(kNoRoster, "account:bob", kNoRoster)) << "a guest's device cannot check";
}

// V062 3: the owner and admins remove a guest; a plain member may not.
TEST(CallGuestsTest, OwnerAndAdminsRemoveGuests) {
  CallRemoveCheck check{.group_call = true, .sender = Admin(), .target = Stranger(), .target_labelled_guest = true};
  EXPECT_EQ(DecideCallRemove(check), CallRemoveVerdict::Remove);
  check.sender = Owner();
  EXPECT_EQ(DecideCallRemove(check), CallRemoveVerdict::Remove);
  check.sender = OpenMember();
  EXPECT_EQ(DecideCallRemove(check), CallRemoveVerdict::Refused);
  check.sender = Stranger();
  EXPECT_EQ(DecideCallRemove(check), CallRemoveVerdict::Refused) << "a guest removes nobody";
  EXPECT_TRUE(MayRemoveFromCall(true, Admin(), Stranger()));
  EXPECT_FALSE(MayRemoveFromCall(true, OpenMember(), Stranger()));
}

// V062 3: members are not removed from a call (they could Join again; removal from the group drops them).
TEST(CallGuestsTest, MembersAreNeverRemovedFromACall) {
  const CallRemoveCheck check{.group_call = true, .sender = Owner(), .target = ManagedMember()};
  EXPECT_EQ(DecideCallRemove(check), CallRemoveVerdict::Refused);
  CallRemoveCheck self = check;
  self.names_local = true;
  EXPECT_EQ(DecideCallRemove(self), CallRemoveVerdict::Refused);
  EXPECT_FALSE(MayRemoveFromCall(true, Owner(), ManagedMember()));
}

// V062 6: in a call with no group nobody removes anyone.
TEST(CallGuestsTest, NobodyRemovesAnyoneFromACallWithNoGroup) {
  const CallRemoveCheck check{.group_call = false, .names_local = true, .target_labelled_guest = true};
  EXPECT_EQ(DecideCallRemove(check), CallRemoveVerdict::Refused);
  EXPECT_FALSE(MayRemoveFromCall(false, Owner(), Stranger()));
}

// V062 3: the guest's own device cannot check and leaves; another guest's device follows the members about a guest.
TEST(CallGuestsTest, AGuestDeviceFollowsARemoval) {
  CallRemoveCheck self{.group_call = true, .names_local = true};
  EXPECT_EQ(DecideCallRemove(self), CallRemoveVerdict::Leave);
  CallRemoveCheck other{.group_call = true, .target_labelled_guest = true};
  EXPECT_EQ(DecideCallRemove(other), CallRemoveVerdict::Remove);
  other.target_labelled_guest = false;
  EXPECT_EQ(DecideCallRemove(other), CallRemoveVerdict::Refused) << "someone not labelled a guest";
}

// V062 1: the guest label: a group's call, brought by someone, not (or no longer) a member here.
TEST(CallGuestsTest, GuestLabel) {
  EXPECT_TRUE(ShowsAsGuest(true, "account:bob", Stranger()));
  EXPECT_TRUE(ShowsAsGuest(true, "account:bob", kNoRoster)) << "a guest's device labels by the row";
  EXPECT_FALSE(ShowsAsGuest(true, "account:bob", ManagedMember())) << "joined the group since";
  EXPECT_FALSE(ShowsAsGuest(true, "", Stranger()));
}

// V062 4: "Add {name} to the group" for whoever may invite to the group, on a guest.
TEST(CallGuestsTest, GroupInviteOfferedToThoseWhoMayInvite) {
  EXPECT_TRUE(OffersGroupInvite(true, Owner(), Stranger()));
  EXPECT_FALSE(OffersGroupInvite(true, Admin(), Stranger()));
  EXPECT_FALSE(OffersGroupInvite(true, Owner(), OpenMember()));
  EXPECT_FALSE(OffersGroupInvite(true, kNoRoster, kNoRoster));
}

// V062 1: `guest` and `brought_by` ride the invite and the roster, both or neither; older peers send neither.
TEST(CallGuestsTest, InviteAndRosterCarryWhoBroughtAGuest) {
  CallInviteDetail invite;
  invite.call_id = "call:1";
  invite.origin_group_id = "group:1";
  invite.guest = true;
  invite.brought_by = "account:bob";
  CallRosterEntry erin;
  erin.identity = "account:erin";
  erin.brought_by = "account:bob";
  invite.participants.push_back(erin);
  const CallInviteDetail back = CallControlCodec::DecodeInvite(CallControlCodec::EncodeInvite(invite).value()).value();
  EXPECT_TRUE(back.guest);
  EXPECT_EQ(back.brought_by, "account:bob");
  ASSERT_EQ(back.participants.size(), 1u);
  EXPECT_EQ(back.participants[0].brought_by, "account:bob");

  CallInviteDetail plain;
  plain.call_id = "call:1";
  const std::string json = CallControlCodec::EncodeInvite(plain).value();
  EXPECT_EQ(json.find("guest"), std::string::npos);
  EXPECT_EQ(json.find("brought_by"), std::string::npos);
  EXPECT_FALSE(CallControlCodec::DecodeInvite(json).value().guest);
  EXPECT_FALSE(CallControlCodec::DecodeInvite(R"({"call_id":"call:1","brought_by":"account:bob"})").value().guest)
      << "brought_by without guest: true";
}

// V062 3: `call_remove {call_id, identity}`, both required; it rides the control lane from someone in the call.
TEST(CallGuestsTest, CallRemoveRoundTrip) {
  const std::string json = CallControlCodec::EncodeRemove({"call:1", "account:erin"}).value();
  const CallRemoveDetail back = CallControlCodec::DecodeRemove(json).value();
  EXPECT_EQ(back.call_id, "call:1");
  EXPECT_EQ(back.identity, "account:erin");
  EXPECT_FALSE(CallControlCodec::DecodeRemove(R"({"call_id":"call:1"})"));
  EXPECT_EQ(CallControlTypeToWire(CallControlType::CallRemove), "call_remove");
  EXPECT_EQ(CallControlTypeFromWire("call_remove"), CallControlType::CallRemove);
  EXPECT_EQ(WhoMaySend(CallControlType::CallRemove), CallControlSender::InCall);
  EXPECT_TRUE(CallControlCodec::TravelsOnLane(CallControlType::CallRemove));
}

} // namespace
} // namespace pbr
