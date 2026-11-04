// A tap on someone in a group's member list or on the group info's member card, on a phone
// (DEV/PLAN-ui-batch8-group-pages.md 2): anyone but the owner goes straight to the profile (Kenneth 2026-11-04);
// the owner gets an action sheet; our own row does nothing. No "transfer owner" here (Kenneth: only when leaving).
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
}

TEST(GroupMemberTapTest, NotTheOwnerGoesStraightToTheProfileContactOrNot) {
  EXPECT_EQ(DecideGroupMemberTap(Member("c1"), false, /*owner=*/false).kind, GroupMemberTap::Kind::OpenProfile);
  EXPECT_EQ(DecideGroupMemberTap(Member(""), false, /*owner=*/false).kind, GroupMemberTap::Kind::OpenProfile);
  EXPECT_TRUE(DecideGroupMemberTap(Member("c1"), false, false).actions.empty());
}

TEST(GroupMemberTapTest, TheOwnerSeesProfileMessageAndRemoveForAContact) {
  const GroupMemberTap tap = DecideGroupMemberTap(Member("c1"), false, /*owner=*/true);
  EXPECT_EQ(tap.kind, GroupMemberTap::Kind::ShowActions);
  EXPECT_EQ(tap.actions, (std::vector<Action>{Action::ViewProfile, Action::Message, Action::Remove}));
}

TEST(GroupMemberTapTest, SomeoneWhoIsNotAContactCanBeAddedNotMessaged) {
  const GroupMemberTap tap = DecideGroupMemberTap(Member(""), false, /*owner=*/true);
  EXPECT_EQ(tap.actions, (std::vector<Action>{Action::ViewProfile, Action::AddContact, Action::Remove}));
}

TEST(GroupMemberTapTest, AnUnreachableContactCanBeInvitedAgain) {
  const GroupMemberTap tap = DecideGroupMemberTap(Member("c1", /*unreachable=*/true), false, /*owner=*/true);
  EXPECT_EQ(tap.actions,
            (std::vector<Action>{Action::ViewProfile, Action::Message, Action::Reinvite, Action::Remove}));
}

TEST(GroupMemberTapTest, SomeoneStillInvitedCannotBeRemovedOnlyInvitedAgain) {
  const GroupMemberTap tap = DecideGroupMemberTap(Member("c1", false, /*invited=*/true), false, /*owner=*/true);
  EXPECT_EQ(tap.actions, (std::vector<Action>{Action::ViewProfile, Action::Message, Action::Reinvite}));
}
