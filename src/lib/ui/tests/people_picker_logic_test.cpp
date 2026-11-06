#include "lib/ui/PeoplePickerLogic.h"

#include <gtest/gtest.h>

using pbr::ComputePeoplePickerCta;
using pbr::PeoplePickerCta;
using pbr::PeoplePickerMode;

TEST(PeoplePickerLogicTest, FreeModeThresholds) {
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::Free, 0), PeoplePickerCta::Disabled);
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::Free, 1), PeoplePickerCta::Message);
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::Free, 2), PeoplePickerCta::CreateGroup);
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::Free, 5), PeoplePickerCta::CreateGroup);
}

TEST(PeoplePickerLogicTest, CallInviteModesRequireSelection) {
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::GroupCall, 0), PeoplePickerCta::Disabled);
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::GroupCall, 1), PeoplePickerCta::StartCall);
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::CallAddGuest, 0), PeoplePickerCta::Disabled);
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::CallAddGuest, 2), PeoplePickerCta::StartCall);
}

TEST(PeoplePickerLogicTest, GroupMembersModeHasNoCta) {
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::GroupMembers, 0), PeoplePickerCta::Disabled);
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::GroupMembers, 3), PeoplePickerCta::Disabled);
}

TEST(PeoplePickerLogicTest, GroupAddMemberNeedsASelectionAndShowsInvitingWhileSending) {
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::GroupAddMember, 0), PeoplePickerCta::Disabled);
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::GroupAddMember, 1), PeoplePickerCta::Invite);
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::GroupAddMember, 4), PeoplePickerCta::Invite);
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::GroupAddMember, 4, true), PeoplePickerCta::Inviting);
  // Sending with nothing selected cannot happen, but must never be clickable.
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::GroupAddMember, 0, true), PeoplePickerCta::Disabled);
}

TEST(PeoplePickerLogicTest, SendingFlagDoesNotChangeOtherModes) {
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::Free, 2, true), PeoplePickerCta::CreateGroup);
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::GroupCall, 1, true), PeoplePickerCta::StartCall);
}

TEST(PeoplePickerLogicTest, GroupFullAtFiftyDisablesTheUnselectedRows) {
  using pbr::GroupAddRowDisabled;
  using pbr::GroupIsFull;
  using pbr::kGroupMemberLimit;
  EXPECT_EQ(kGroupMemberLimit, 50);
  EXPECT_FALSE(GroupIsFull(10, 39));
  EXPECT_TRUE(GroupIsFull(10, 40));
  EXPECT_TRUE(GroupIsFull(50, 0));
  // Below the limit nothing is disabled; at the limit only the rows not yet picked are.
  EXPECT_FALSE(GroupAddRowDisabled(10, 39, false));
  EXPECT_TRUE(GroupAddRowDisabled(10, 40, false));
  EXPECT_FALSE(GroupAddRowDisabled(10, 40, true));
}

TEST(PeoplePickerLogicTest, AddMemberCandidatesLeaveOutTheRosterAndSelf) {
  using pbr::AddMemberCandidate;
  const std::vector<AddMemberCandidate> contacts = {
      {"c1", "acct:alice"}, {"c2", "acct:bob"}, {"c3", "acct:me"}, {"c4", ""}, {"c5", "acct:carol"}};
  const std::unordered_set<std::string> roster = {"acct:bob", "acct:me"};
  const std::vector<std::string> ids = pbr::AddMemberCandidateIds(contacts, roster, "acct:me");
  EXPECT_EQ(ids, (std::vector<std::string>{"c1", "c5"}));
}

TEST(PeoplePickerLogicTest, AddMemberCandidatesLeaveOutPeopleAlreadyInvited) {
  using pbr::AddMemberCandidate;
  const std::vector<AddMemberCandidate> contacts = {{"c1", "acct:alice"}, {"c2", "acct:bob"}};
  const std::vector<std::string> ids = pbr::AddMemberCandidateIds(contacts, {}, "acct:me", {"acct:alice"});
  EXPECT_EQ(ids, (std::vector<std::string>{"c2"}));
}

TEST(PeoplePickerLogicTest, InviteNeedsContactHintShowsWhenTheButtonCreatesOrInvites) {
  using pbr::ShowInviteNeedsContactHint;
  EXPECT_TRUE(ShowInviteNeedsContactHint(ComputePeoplePickerCta(PeoplePickerMode::FromDm, 1)));
  EXPECT_TRUE(ShowInviteNeedsContactHint(ComputePeoplePickerCta(PeoplePickerMode::Free, 2)));
  EXPECT_TRUE(ShowInviteNeedsContactHint(ComputePeoplePickerCta(PeoplePickerMode::GroupAddMember, 1)));
  EXPECT_TRUE(ShowInviteNeedsContactHint(ComputePeoplePickerCta(PeoplePickerMode::GroupAddMember, 1, true)));
  // One person picked in Free mode is a plain chat; calls and the read-only list invite no one to a group.
  EXPECT_FALSE(ShowInviteNeedsContactHint(ComputePeoplePickerCta(PeoplePickerMode::Free, 1)));
  EXPECT_FALSE(ShowInviteNeedsContactHint(ComputePeoplePickerCta(PeoplePickerMode::Free, 0)));
  EXPECT_FALSE(ShowInviteNeedsContactHint(ComputePeoplePickerCta(PeoplePickerMode::FromDm, 0)));
  EXPECT_FALSE(ShowInviteNeedsContactHint(ComputePeoplePickerCta(PeoplePickerMode::GroupCall, 2)));
  EXPECT_FALSE(ShowInviteNeedsContactHint(ComputePeoplePickerCta(PeoplePickerMode::GroupMembers, 2)));
}

// G022: "Make a group…" — everyone in the call but us, each once, all picked; Next with one or more.
TEST(PeoplePickerLogicTest, MakeAGroupFromACallPicksEveryoneElseInIt) {
  EXPECT_EQ(pbr::CallMakeGroupPick({"account:ann", "account:me", "account:bo", "account:ann", "", "account:cy"},
                                   "account:me"),
            (std::vector<std::string>{"account:ann", "account:bo", "account:cy"}));
  EXPECT_TRUE(pbr::CallMakeGroupPick({"account:me"}, "account:me").empty());
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::CallMakeGroup, 0), PeoplePickerCta::Disabled)
      << "everyone unticked";
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::CallMakeGroup, 1), PeoplePickerCta::CreateGroup);
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::CallMakeGroup, 4), PeoplePickerCta::CreateGroup);
  EXPECT_EQ(pbr::MinGroupPick(PeoplePickerMode::CallMakeGroup), 1u);
  EXPECT_EQ(pbr::MinGroupPick(PeoplePickerMode::Free), 2u);
  // People met in the call need not have us as a contact: no "must have added you" hint.
  EXPECT_FALSE(pbr::ShowInviteNeedsContactHint(PeoplePickerCta::CreateGroup, PeoplePickerMode::CallMakeGroup));
  EXPECT_TRUE(pbr::ShowInviteNeedsContactHint(PeoplePickerCta::CreateGroup, PeoplePickerMode::FromDm));
}

// G022: the default name lists the people (the maker first), three at most, then counts the rest.
TEST(PeoplePickerLogicTest, ACallGroupIsNamedAfterThePeopleInIt) {
  EXPECT_EQ(pbr::DefaultCallGroupTitle({"Ann", "Bo", "Cy"}, ", ", "Group chat"), "Ann, Bo, Cy");
  EXPECT_EQ(pbr::DefaultCallGroupTitle({"Ann", "Bo", "Cy", "Di", "Ed"}, ", ", "Group chat"), "Ann, Bo, Cy +2");
  EXPECT_EQ(pbr::DefaultCallGroupTitle({"", "Bo", "Cy"}, ", ", "Group chat"), "Bo, Cy") << "a name we lack is skipped";
  EXPECT_EQ(pbr::DefaultCallGroupTitle({"安", "博"}, "、", "群聊"), "安、博");
  EXPECT_EQ(pbr::DefaultCallGroupTitle({"", ""}, ", ", "Group chat"), "Group chat");
}
