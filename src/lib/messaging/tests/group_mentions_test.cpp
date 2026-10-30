#include "lib/messaging/GroupMentions.h"

#include <gtest/gtest.h>

namespace {

using namespace pbr;

GroupRosterMember Member(const std::string& id, const MemberRole role = MemberRole::Member) {
  GroupRosterMember member;
  member.member_identity = id;
  member.role = role;
  return member;
}

MentionRange Range(const std::string& id, const uint32_t start, const uint32_t end) {
  MentionRange range;
  range.account_id = id;
  range.start = start;
  range.end = end;
  return range;
}

std::string Json(const std::vector<MentionRange>& mentions, const std::optional<MentionRange>& all = std::nullopt) {
  MentionsPayload payload;
  payload.mentions = mentions;
  payload.all = all;
  return BuildMentionsPayloadJson("@bob @carol hi", payload);
}

const std::vector<GroupRosterMember> kRoster{Member("owner", MemberRole::Owner), Member("bob"), Member("carol"),
                                             Member("dave")};

} // namespace

TEST(GroupMentionsTest, MemberIdIsMentioned) {
  const std::string json = Json({Range("bob", 0, 4)});
  EXPECT_TRUE(MentionsMe(json, "bob", "carol", kRoster));
  EXPECT_FALSE(MentionsMe(json, "dave", "carol", kRoster)) << "not in the list";
}

TEST(GroupMentionsTest, NonMemberIdDoesNotCount) {
  const std::string json = Json({Range("mallory", 0, 4)});
  EXPECT_FALSE(MentionsMe(json, "mallory", "carol", kRoster));
  EXPECT_TRUE(EligibleMentions(json, "carol", kRoster).empty());
}

TEST(GroupMentionsTest, AllFromTheOwnerMentionsEveryone) {
  const std::string json = Json({}, Range("", 0, 4));
  for (const char* self : {"bob", "carol", "dave"}) {
    EXPECT_TRUE(MentionsMe(json, self, "owner", kRoster)) << self;
  }
  const auto eligible = EligibleMentions(json, "owner", kRoster);
  ASSERT_EQ(eligible.size(), 1u);
  EXPECT_EQ(eligible[0].start, 0u);
}

TEST(GroupMentionsTest, AllFromANonOwnerDoesNot) {
  const std::string json = Json({}, Range("", 0, 4));
  EXPECT_FALSE(MentionsMe(json, "bob", "carol", kRoster));
  EXPECT_TRUE(EligibleMentions(json, "carol", kRoster).empty());
  EXPECT_FALSE(MentionsMe(json, "bob", "mallory", kRoster));
}

TEST(GroupMentionsTest, NoMentionsMeansNotMentioned) {
  EXPECT_FALSE(MentionsMe(R"({"text":"hi"})", "bob", "owner", kRoster));
  EXPECT_FALSE(MentionsMe("not json", "bob", "owner", kRoster));
  EXPECT_FALSE(MentionsMe(Json({Range("bob", 0, 4)}), "", "owner", kRoster));
}

TEST(GroupMentionsTest, LateRosterMakesHighlightsAppear) {
  const std::string json = Json({Range("bob", 0, 4)}, Range("", 5, 11));
  std::vector<GroupRosterMember> early{Member("bob")};  // owner not synced yet
  EXPECT_TRUE(EligibleMentions(json, "owner", early).size() == 1u) << "bob is on the roster, the sender is not";
  EXPECT_FALSE(MentionsMe(Json({}, Range("", 5, 11)), "bob", "owner", early));

  std::vector<GroupRosterMember> empty;
  EXPECT_TRUE(EligibleMentions(json, "owner", empty).empty());

  EXPECT_EQ(EligibleMentions(json, "owner", kRoster).size(), 2u);
  EXPECT_TRUE(MentionsMe(Json({}, Range("", 5, 11)), "bob", "owner", kRoster));
}

TEST(GroupMentionsTest, RangeFitsText) {
  const std::string text = "@bob \xE4\xBD\xA0 x";  // @bob, space, 你 (5..8), space
  EXPECT_TRUE(MentionRangeFitsText(text, 0, 4));
  EXPECT_FALSE(MentionRangeFitsText(text, 0, 0));
  EXPECT_FALSE(MentionRangeFitsText(text, 1, 4));
  EXPECT_FALSE(MentionRangeFitsText(text, 0, 100));
  EXPECT_FALSE(MentionRangeFitsText("\xE4\xBD\xA0@", 1, 4));
  EXPECT_TRUE(MentionRangeFitsText("@\xE4\xBD\xA0", 0, 4));
  EXPECT_FALSE(MentionRangeFitsText("@\xE4\xBD\xA0", 0, 2)) << "ends inside the character";
}
