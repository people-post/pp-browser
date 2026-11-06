// Who may do what in a group (G017): the one table every check asks.
#include "lib/messaging/GroupPermissions.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

constexpr MemberRole kOwner = MemberRole::Owner;
constexpr MemberRole kAdmin = MemberRole::Admin;
constexpr MemberRole kMember = MemberRole::Member;
constexpr GroupPreset kOpen = GroupPreset::Open;
constexpr GroupPreset kManaged = GroupPreset::Managed;

TEST(GroupPermissionsTest, AnOpenGroupLetsAnyMemberBringPeopleInAndRename) {
  for (const GroupAction action :
       {GroupAction::Invite, GroupAction::MakeInviteLink, GroupAction::BringCallGuest, GroupAction::Rename}) {
    for (const MemberRole role : {kOwner, kAdmin, kMember}) {
      EXPECT_TRUE(GroupAllows(action, role, kOpen));
    }
  }
}

TEST(GroupPermissionsTest, AManagedGroupLeavesThatToTheOwnerAndAdmins) {
  for (const GroupAction action :
       {GroupAction::Invite, GroupAction::MakeInviteLink, GroupAction::BringCallGuest, GroupAction::Rename}) {
    EXPECT_TRUE(GroupAllows(action, kOwner, kManaged));
    EXPECT_TRUE(GroupAllows(action, kAdmin, kManaged));
    EXPECT_FALSE(GroupAllows(action, kMember, kManaged));
  }
}

TEST(GroupPermissionsTest, TheOwnerRemovesAnyoneButItselfAnAdminOnlyMembers) {
  for (const GroupPreset preset : {kOpen, kManaged}) {
    EXPECT_TRUE(GroupAllows(GroupAction::Remove, kOwner, preset, kMember));
    EXPECT_TRUE(GroupAllows(GroupAction::Remove, kOwner, preset, kAdmin));
    EXPECT_FALSE(GroupAllows(GroupAction::Remove, kOwner, preset, kOwner));
    EXPECT_TRUE(GroupAllows(GroupAction::Remove, kAdmin, preset, kMember));
    EXPECT_FALSE(GroupAllows(GroupAction::Remove, kAdmin, preset, kAdmin));
    EXPECT_FALSE(GroupAllows(GroupAction::Remove, kAdmin, preset, kOwner));
    EXPECT_FALSE(GroupAllows(GroupAction::Remove, kMember, preset, kMember));
  }
}

TEST(GroupPermissionsTest, KeepingOrderIsTheOwnersAndAdminsInEveryGroup) {
  for (const GroupAction action : {GroupAction::Readmit, GroupAction::RemoveCallGuest, GroupAction::MentionEveryone}) {
    for (const GroupPreset preset : {kOpen, kManaged}) {
      EXPECT_TRUE(GroupAllows(action, kOwner, preset));
      EXPECT_TRUE(GroupAllows(action, kAdmin, preset));
      EXPECT_FALSE(GroupAllows(action, kMember, preset));
    }
  }
}

TEST(GroupPermissionsTest, WhoIsInChargeIsTheOwnersAlone) {
  for (const GroupAction action : {GroupAction::SetPreset, GroupAction::Transfer, GroupAction::Dissolve}) {
    EXPECT_TRUE(GroupAllows(action, kOwner, kOpen));
    EXPECT_FALSE(GroupAllows(action, kAdmin, kOpen));
    EXPECT_FALSE(GroupAllows(action, kMember, kOpen));
  }
  EXPECT_TRUE(GroupAllows(GroupAction::SetRole, kOwner, kOpen, kMember));
  EXPECT_TRUE(GroupAllows(GroupAction::SetRole, kOwner, kOpen, kAdmin));
  EXPECT_FALSE(GroupAllows(GroupAction::SetRole, kOwner, kOpen, kOwner)) << "ownership moves by transfer only";
  EXPECT_FALSE(GroupAllows(GroupAction::SetRole, kAdmin, kOpen, kMember));
}

TEST(GroupPermissionsTest, EveryoneSends) {
  for (const MemberRole role : {kOwner, kAdmin, kMember}) {
    EXPECT_TRUE(GroupAllows(GroupAction::Send, role, kManaged));
  }
}

} // namespace
} // namespace pbr
