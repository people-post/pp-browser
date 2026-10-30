#include "common/directory/ShortRelayId.h"
#include "feature/conversations/ChatPresenter.h"

#include <gtest/gtest.h>

using namespace pbr;

namespace {

GroupRosterMember Member(const std::string& identity, const MemberRole role = MemberRole::Member) {
  GroupRosterMember member;
  member.member_identity = identity;
  member.role = role;
  return member;
}

Contact SavedContact(const std::string& id, const std::string& display_name, const std::string& nickname = "") {
  Contact contact;
  contact.id = id;
  contact.display_name = display_name;
  contact.server_nickname = nickname;
  return contact;
}

} // namespace

TEST(GroupMemberRowTest, ContactNameWinsOverServerNickname) {
  const GroupMemberRow row =
      BuildGroupMemberRow(Member("acct:alice"), SavedContact("c1", "Alice at work", "alice77"), false);
  EXPECT_EQ(row.title, "Alice at work");
  EXPECT_EQ(row.contact_id, "c1");
  EXPECT_TRUE(row.IsContact());
}

TEST(GroupMemberRowTest, ServerNicknameWhenTheContactHasNoName) {
  const GroupMemberRow row = BuildGroupMemberRow(Member("acct:alice"), SavedContact("c1", "", "alice77"), false);
  EXPECT_EQ(row.title, "alice77");
}

TEST(GroupMemberRowTest, NonContactShowsTheShortIdAndIsNotAContact) {
  const std::string identity = "acct:0123456789abcdef0123456789abcdef";
  const GroupMemberRow row = BuildGroupMemberRow(Member(identity), std::nullopt, false);
  EXPECT_EQ(row.title, ShortRelayId(identity));
  EXPECT_FALSE(row.title.empty());
  EXPECT_FALSE(row.IsContact());
  EXPECT_FALSE(row.is_owner);
  EXPECT_FALSE(row.unreachable);
}

TEST(GroupMemberRowTest, NonContactShowsTheNicknameOnTheirCard) {
  const GroupMemberRow row = BuildGroupMemberRow(Member("acct:k"), std::nullopt, false, "KEN-WIN");
  EXPECT_EQ(row.title, "KEN-WIN");
  EXPECT_FALSE(row.IsContact());
}

TEST(GroupMemberRowTest, OwnerAndUnreachableBadges) {
  const GroupMemberRow owner =
      BuildGroupMemberRow(Member("acct:o", MemberRole::Owner), SavedContact("c1", "Olga"), false);
  EXPECT_TRUE(owner.is_owner);
  EXPECT_FALSE(owner.unreachable);
  const GroupMemberRow gone = BuildGroupMemberRow(Member("acct:g"), SavedContact("c2", "Gus"), true);
  EXPECT_FALSE(gone.is_owner);
  EXPECT_TRUE(gone.unreachable);
}

TEST(GroupMemberRowTest, UnreachableFirstThenOwnerThenByName) {
  std::vector<GroupMemberRow> rows = {
      BuildGroupMemberRow(Member("acct:b"), SavedContact("cb", "Bea"), false),
      BuildGroupMemberRow(Member("acct:o", MemberRole::Owner), SavedContact("co", "Zed"), false),
      BuildGroupMemberRow(Member("acct:u2"), SavedContact("cu2", "Yan"), true),
      BuildGroupMemberRow(Member("acct:a"), SavedContact("ca", "Abe"), false),
      BuildGroupMemberRow(Member("acct:u1"), SavedContact("cu1", "Xia"), true),
  };
  SortGroupMemberRows(rows);
  std::vector<std::string> titles;
  for (const GroupMemberRow& row : rows) {
    titles.push_back(row.title);
  }
  EXPECT_EQ(titles, (std::vector<std::string>{"Xia", "Yan", "Zed", "Abe", "Bea"}));
}

namespace {
PendingGroupInvite Invite(const std::string& invitee, const int64_t created_at) {
  PendingGroupInvite invite;
  invite.invitee_identity = invitee;
  invite.created_at = created_at;
  invite.outbound = true;
  return invite;
}
} // namespace

TEST(GroupInvitedRowTest, FreshInviteIsWaiting) {
  const auto row = BuildInvitedRow(Invite("acct:bob", 1000), {}, SavedContact("c2", "Bob"), 1000 + 60 * 1000);
  ASSERT_TRUE(row.has_value());
  EXPECT_TRUE(row->invited);
  EXPECT_FALSE(row->invited_stale);
  EXPECT_EQ(row->title, "Bob");
  EXPECT_EQ(row->contact_id, "c2");
  EXPECT_FALSE(row->unreachable);
}

TEST(GroupInvitedRowTest, InviteOlderThanADayIsStale) {
  const auto row = BuildInvitedRow(Invite("acct:bob", 0), {}, std::nullopt, kInviteStaleAfterMs + 1, "Bobby");
  ASSERT_TRUE(row.has_value());
  EXPECT_TRUE(row->invited_stale);
  EXPECT_EQ(row->title, "Bobby");
  const auto edge = BuildInvitedRow(Invite("acct:bob", 0), {}, std::nullopt, kInviteStaleAfterMs);
  ASSERT_TRUE(edge.has_value());
  EXPECT_FALSE(edge->invited_stale);
}

TEST(GroupInvitedRowTest, InviteeAlreadyOnTheRosterGetsNoRow) {
  const std::unordered_set<std::string> roster = {"acct:bob"};
  EXPECT_FALSE(BuildInvitedRow(Invite("acct:bob", 0), roster, std::nullopt, 10).has_value());
}
