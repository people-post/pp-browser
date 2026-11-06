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
  EXPECT_EQ(DecideGroupMemberTap(Member("c1"), /*is_self=*/true, /*owner=*/true).kind, GroupMemberTap::Kind::Nothing);
  EXPECT_EQ(DecideGroupMemberTap(Member(""), /*is_self=*/true, /*owner=*/false).kind, GroupMemberTap::Kind::Nothing);
  EXPECT_TRUE(DecideGroupMemberTap(Member("c1"), true, true).admin_actions.empty());
}

TEST(GroupMemberTapTest, NotTheOwnerGoesStraightToTheProfileWithoutTheAdminCard) {
  EXPECT_EQ(DecideGroupMemberTap(Member("c1"), false, /*owner=*/false).kind, GroupMemberTap::Kind::OpenProfile);
  EXPECT_EQ(DecideGroupMemberTap(Member(""), false, /*owner=*/false).kind, GroupMemberTap::Kind::OpenProfile);
  EXPECT_TRUE(DecideGroupMemberTap(Member("c1", /*unreachable=*/true), false, false).admin_actions.empty());
  EXPECT_TRUE(DecideGroupMemberTap(Member("c1", false, /*invited=*/true), false, false).admin_actions.empty());
}

TEST(GroupMemberTapTest, TheOwnerAlsoOpensTheProfileWithRemoveInTheCard) {
  const GroupMemberTap tap = DecideGroupMemberTap(Member("c1"), false, /*owner=*/true);
  EXPECT_EQ(tap.kind, GroupMemberTap::Kind::OpenProfile);
  EXPECT_EQ(tap.admin_actions, (std::vector<Action>{Action::MakeAdmin, Action::Remove}));
  EXPECT_EQ(tap.reinvite_note, GroupReinviteNote::None);
}

TEST(GroupMemberTapTest, SomeoneWhoIsNotAContactCanBeRemovedNotInvitedAgain) {
  const GroupMemberTap tap = DecideGroupMemberTap(Member("", /*unreachable=*/true), false, /*owner=*/true);
  EXPECT_EQ(tap.kind, GroupMemberTap::Kind::OpenProfile);
  EXPECT_EQ(tap.admin_actions, (std::vector<Action>{Action::MakeAdmin, Action::Remove}));
}

TEST(GroupMemberTapTest, AnUnreachableContactCanBeInvitedAgainNotedCantBeReached) {
  const GroupMemberTap tap = DecideGroupMemberTap(Member("c1", /*unreachable=*/true), false, /*owner=*/true);
  EXPECT_EQ(tap.admin_actions, (std::vector<Action>{Action::Reinvite, Action::MakeAdmin, Action::Remove}));
  EXPECT_EQ(tap.reinvite_note, GroupReinviteNote::Unreachable);
}

TEST(GroupMemberTapTest, TheOwnerTakesAnAdminsRoleBack) {
  GroupMemberRow admin = Member("c1");
  admin.is_admin = true;
  EXPECT_EQ(DecideGroupMemberTap(admin, false, /*owner=*/true).admin_actions,
            (std::vector<Action>{Action::RemoveAdmin, Action::Remove}));
  EXPECT_TRUE(DecideGroupMemberTap(admin, false, /*owner=*/false).admin_actions.empty())
      << "an admin's own card is the owner's";
}

TEST(GroupMemberTapTest, SomeoneStillInvitedCannotBeRemovedOnlyInvitedAgainNotedNotJoined) {
  const GroupMemberTap tap = DecideGroupMemberTap(Member("c1", false, /*invited=*/true), false, /*owner=*/true);
  EXPECT_EQ(tap.admin_actions, (std::vector<Action>{Action::Reinvite}));
  EXPECT_EQ(tap.reinvite_note, GroupReinviteNote::NotJoined);
}
