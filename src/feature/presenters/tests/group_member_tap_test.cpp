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

// G020 6: the bulk remedies on the person's page — "Remove everyone {name} added…" for the owner and admins when they
// added someone still in, "Re-invite everyone {name} removed…" for the owner when they removed someone still out.
TEST(GroupMemberTapTest, TheBulkRemediesShowByRoleAndWhenThereIsSomeoneToActOn) {
  GroupMemberRow bob = Member("c1");
  bob.added_count = 2;
  bob.removed_count = 1;
  EXPECT_EQ(DecideGroupMemberTap(bob, false, MemberRole::Owner).admin_actions,
            (std::vector<Action>{Action::MakeAdmin, Action::Remove, Action::RemoveEveryoneAdded,
                                 Action::ReinviteEveryoneRemoved}));
  EXPECT_EQ(DecideGroupMemberTap(bob, false, MemberRole::Admin).admin_actions,
            (std::vector<Action>{Action::Remove, Action::RemoveEveryoneAdded}))
      << "an admin removes; re-inviting whom someone removed is the owner's";
  EXPECT_TRUE(DecideGroupMemberTap(bob, false, MemberRole::Member).admin_actions.empty()) << "a member removes no one";

  GroupMemberRow quiet = Member("c1");
  EXPECT_EQ(DecideGroupMemberTap(quiet, false, MemberRole::Owner).admin_actions,
            (std::vector<Action>{Action::MakeAdmin, Action::Remove}))
      << "nobody they added or removed: no bulk rows";
  GroupMemberRow invited = bob;
  invited.invited = true;
  for (const Action a : DecideGroupMemberTap(invited, false, MemberRole::Owner).admin_actions) {
    EXPECT_NE(a, Action::RemoveEveryoneAdded) << "someone not in yet added no one";
  }
}

// G019 5 / G020 6: who added each member, by name, and how many each added and removed.
TEST(GroupMemberTapTest, TheMemberRowsSayWhoAddedThemAndHowManyEachBroughtIn) {
  std::vector<GroupRosterMember> roster{{.member_identity = "acct:alice", .role = MemberRole::Owner},
                                        {.member_identity = "acct:bob", .added_by = "acct:alice"},
                                        {.member_identity = "acct:erin", .added_by = "acct:bob"},
                                        {.member_identity = "acct:frank", .added_by = "acct:bob"},
                                        {.member_identity = "acct:gus", .added_by = "acct:gone"}};
  std::vector<GroupMemberRow> rows;
  for (const GroupRosterMember& m : roster) {
    GroupMemberRow row;
    row.identity = m.member_identity;
    row.title = m.member_identity.substr(5);
    rows.push_back(row);
  }
  FillGroupAccountability(rows, roster, [](const std::string& account) { return account == "acct:bob" ? 3u : 0u; });
  EXPECT_TRUE(rows[0].added_by.empty()) << "the group's first members were added by no one";
  EXPECT_EQ(rows[1].added_by_title, "alice");
  EXPECT_EQ(rows[1].added_count, 2u);
  EXPECT_EQ(rows[1].removed_count, 3u);
  EXPECT_EQ(rows[2].added_by, "acct:bob");
  EXPECT_EQ(rows[2].added_by_title, "bob");
  EXPECT_EQ(rows[0].added_count, 1u);
  EXPECT_FALSE(rows[4].added_by_title.empty()) << "someone who left still shows, by a short id";
}
