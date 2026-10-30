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
