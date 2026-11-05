#include "feature/presenters/ChatPresenter.h"

#include <gtest/gtest.h>

using namespace pbr;

namespace {

GroupMemberRow Row(const std::string& identity, const std::string& title, const bool owner = false,
                   const bool unreachable = false) {
  GroupMemberRow row;
  row.identity = identity;
  row.title = title;
  row.is_owner = owner;
  row.unreachable = unreachable;
  return row;
}

GroupInfoFacts Facts(const bool owner) {
  GroupInfoFacts facts;
  facts.thread_id = "t1";
  facts.group_id = "g1";
  facts.title = "Weekend hike";
  facts.is_owner = owner;
  facts.self_identity = "acct:me";
  facts.members = {Row("acct:me", "You", owner), Row("acct:b", "Bea"), Row("acct:a", "Ann"),
                   Row("acct:o", "Olga", !owner), Row("acct:c", "Cid"), Row("acct:d", "Dan"), Row("acct:e", "Eve")};
  return facts;
}

std::vector<std::string> Titles(const std::vector<GroupMemberRow>& rows) {
  std::vector<std::string> out;
  for (const GroupMemberRow& row : rows) {
    out.push_back(row.title);
  }
  return out;
}

} // namespace

TEST(GroupInfoScreenTest, OwnerSeesAddDissolveAndRenamesForEveryone) {
  const GroupInfoScreen screen = BuildGroupInfoScreen(Facts(/*owner=*/true));
  EXPECT_TRUE(screen.is_owner);
  EXPECT_TRUE(screen.show_members);
  EXPECT_TRUE(screen.show_calls);
  EXPECT_TRUE(screen.show_rename);
  EXPECT_TRUE(screen.rename_for_everyone);
  EXPECT_TRUE(screen.show_add_member);
  EXPECT_TRUE(screen.show_leave);
  EXPECT_TRUE(screen.show_dissolve);
  EXPECT_FALSE(screen.show_restore_name);
}

TEST(GroupInfoScreenTest, MemberRenamesForThemselvesAndCannotAddOrDissolve) {
  const GroupInfoScreen screen = BuildGroupInfoScreen(Facts(/*owner=*/false));
  EXPECT_FALSE(screen.is_owner);
  EXPECT_TRUE(screen.show_rename);
  EXPECT_FALSE(screen.rename_for_everyone);
  EXPECT_FALSE(screen.show_add_member);
  EXPECT_TRUE(screen.show_leave);
  EXPECT_FALSE(screen.show_dissolve);
}

TEST(GroupInfoScreenTest, FirstFiveWithTheOwnerFirstAndTheWholeCount) {
  const GroupInfoScreen screen = BuildGroupInfoScreen(Facts(/*owner=*/false));
  EXPECT_EQ(screen.member_count, 7); // "See all 7 members" counts everyone, this device too
  ASSERT_EQ(screen.preview.size(), kGroupInfoPreviewMembers);
  EXPECT_EQ(Titles(screen.preview), (std::vector<std::string>{"Olga", "Ann", "Bea", "Cid", "Dan"}));
  EXPECT_EQ(screen.self_identity, "acct:me");
}

TEST(GroupInfoScreenTest, TheOwnerHeadsTheCardEvenBeforeUnreachableMembers) {
  GroupInfoFacts facts = Facts(/*owner=*/true);
  facts.members.push_back(Row("acct:z", "Zed", false, /*unreachable=*/true));
  const GroupInfoScreen screen = BuildGroupInfoScreen(facts);
  ASSERT_GE(screen.preview.size(), 2u);
  EXPECT_EQ(screen.preview[0].title, "You");
  EXPECT_EQ(screen.preview[1].title, "Zed"); // then the list's own order: the unreachable first, to re-invite
}

TEST(GroupInfoScreenTest, RenamedForMeOffersRestoringTheName) {
  GroupInfoFacts facts = Facts(/*owner=*/false);
  facts.has_local_title = true;
  EXPECT_TRUE(BuildGroupInfoScreen(facts).show_restore_name);
}

TEST(GroupInfoScreenTest, DissolvedGroupShowsOnlyNameAndMute) {
  GroupInfoFacts facts = Facts(/*owner=*/true);
  facts.dissolved = true;
  facts.muted = true;
  facts.has_local_title = true;
  const GroupInfoScreen screen = BuildGroupInfoScreen(facts);
  EXPECT_TRUE(screen.dissolved);
  EXPECT_TRUE(screen.muted);
  EXPECT_EQ(screen.title, "Weekend hike");
  EXPECT_FALSE(screen.show_members);
  EXPECT_FALSE(screen.show_calls);
  EXPECT_FALSE(screen.show_rename);
  EXPECT_FALSE(screen.show_restore_name);
  EXPECT_FALSE(screen.show_add_member);
  EXPECT_FALSE(screen.show_leave);
  EXPECT_FALSE(screen.show_dissolve);
  EXPECT_TRUE(screen.preview.empty());
  EXPECT_EQ(screen.member_count, 0);
}
