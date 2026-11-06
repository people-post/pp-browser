// A tap on someone in a group's member list, the group info's member card or their avatar in the chat, on a phone
// (Kenneth 2026-11-04, ui-mockups/09-sheets-unify ③): everyone, the owner too, opens the person's profile; the owner
// finds a "Group admin" card at its bottom (Re-invite with a note, Make admin / Remove as admin, Remove); our own row
// does nothing. No "transfer
// owner" here (Kenneth: only when leaving).
#include "feature/presenters/ChatPresenter.h"

#include <gtest/gtest.h>

using namespace pbr;
using Action = GroupMemberAction;

namespace {

GroupMemberRow Member(const std::string& contact_id, const bool unreachable = false, const bool invited = false) {
  GroupMemberRow row;
  row.identity = "acct:x";
  row.title = "Ann";
  row.contact_id = contact_id;
  row.unreachable = unreachable;
  row.invited = invited;
  return row;
}

} // namespace

TEST(GroupMemberTapTest, OurOwnRowDoesNothing) {
  EXPECT_EQ(DecideGroupMemberTap(Member("c1"), /*is_self=*/true, MemberRole::Owner).kind,
            GroupMemberTap::Kind::Nothing);
  EXPECT_EQ(DecideGroupMemberTap(Member(""), /*is_self=*/true, MemberRole::Member).kind, GroupMemberTap::Kind::Nothing);
  EXPECT_TRUE(DecideGroupMemberTap(Member("c1"), true, MemberRole::Owner).admin_actions.empty());
}

TEST(GroupMemberTapTest, NotTheOwnerGoesStraightToTheProfileWithoutTheAdminCard) {
  EXPECT_EQ(DecideGroupMemberTap(Member("c1"), false, MemberRole::Member).kind, GroupMemberTap::Kind::OpenProfile);
  EXPECT_EQ(DecideGroupMemberTap(Member(""), false, MemberRole::Member).kind, GroupMemberTap::Kind::OpenProfile);
  EXPECT_TRUE(
      DecideGroupMemberTap(Member("c1", /*unreachable=*/true), false, MemberRole::Member).admin_actions.empty());
  EXPECT_TRUE(
      DecideGroupMemberTap(Member("c1", false, /*invited=*/true), false, MemberRole::Member).admin_actions.empty());
}

TEST(GroupMemberTapTest, TheOwnerAlsoOpensTheProfileWithRemoveInTheCard) {
  const GroupMemberTap tap = DecideGroupMemberTap(Member("c1"), false, MemberRole::Owner);
  EXPECT_EQ(tap.kind, GroupMemberTap::Kind::OpenProfile);
  EXPECT_EQ(tap.admin_actions, (std::vector<Action>{Action::MakeAdmin, Action::Remove}));
  EXPECT_EQ(tap.reinvite_note, GroupReinviteNote::None);
}

TEST(GroupMemberTapTest, SomeoneWhoIsNotAContactCanBeRemovedNotInvitedAgain) {
  const GroupMemberTap tap = DecideGroupMemberTap(Member("", /*unreachable=*/true), false, MemberRole::Owner);
  EXPECT_EQ(tap.kind, GroupMemberTap::Kind::OpenProfile);
  EXPECT_EQ(tap.admin_actions, (std::vector<Action>{Action::MakeAdmin, Action::Remove}));
}

TEST(GroupMemberTapTest, AnUnreachableContactCanBeInvitedAgainNotedCantBeReached) {
  const GroupMemberTap tap = DecideGroupMemberTap(Member("c1", /*unreachable=*/true), false, MemberRole::Owner);
  EXPECT_EQ(tap.admin_actions, (std::vector<Action>{Action::Reinvite, Action::MakeAdmin, Action::Remove}));
  EXPECT_EQ(tap.reinvite_note, GroupReinviteNote::Unreachable);
}

TEST(GroupMemberTapTest, TheOwnerTakesAnAdminsRoleBack) {
  GroupMemberRow admin = Member("c1");
  admin.is_admin = true;
  EXPECT_EQ(DecideGroupMemberTap(admin, false, MemberRole::Owner).admin_actions,
            (std::vector<Action>{Action::RemoveAdmin, Action::Remove}));
  EXPECT_TRUE(DecideGroupMemberTap(admin, false, MemberRole::Member).admin_actions.empty())
      << "an admin's own card is the owner's";
}

// G017 / G020: an admin removes members — from the same card — but neither other admins nor the owner, and makes no
// admins.
TEST(GroupMemberTapTest, AnAdminMayRemoveMembersOnly) {
  EXPECT_EQ(DecideGroupMemberTap(Member("c1"), false, MemberRole::Admin).admin_actions,
            (std::vector<Action>{Action::Remove}));
  EXPECT_EQ(DecideGroupMemberTap(Member("c1"), false, MemberRole::Admin, GroupPreset::Managed).admin_actions,
            (std::vector<Action>{Action::Remove}))
      << "removal is the same in either preset";
  GroupMemberRow admin = Member("c1");
  admin.is_admin = true;
  EXPECT_TRUE(DecideGroupMemberTap(admin, false, MemberRole::Admin).admin_actions.empty());
  GroupMemberRow owner = Member("c1");
  owner.is_owner = true;
  EXPECT_TRUE(DecideGroupMemberTap(owner, false, MemberRole::Admin).admin_actions.empty());
  EXPECT_TRUE(
      DecideGroupMemberTap(Member("c1", false, /*invited=*/true), false, MemberRole::Admin).admin_actions.empty())
      << "someone invited is not in the group yet";
  EXPECT_FALSE(MayRemoveGroupMember(Member("c1"), MemberRole::Member)) << "a member removes no one";
}

TEST(GroupMemberTapTest, SomeoneStillInvitedCannotBeRemovedOnlyInvitedAgainNotedNotJoined) {
  const GroupMemberTap tap = DecideGroupMemberTap(Member("c1", false, /*invited=*/true), false, MemberRole::Owner);
  EXPECT_EQ(tap.admin_actions, (std::vector<Action>{Action::Reinvite}));
  EXPECT_EQ(tap.reinvite_note, GroupReinviteNote::NotJoined);
}
