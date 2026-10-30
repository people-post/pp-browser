#include "lib/people/AccountStateCodec.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

TEST(AccountStateCodecTest, GroupRoundTripsWithMembersAndRosterStatement) {
  AccountStateGroup group;
  group.group_id = "group:hike";
  group.title = "Weekend hike";
  group.owner_identity = "account:alice";
  group.roster_epoch = 4;
  group.title_version = 2;
  group.policy_json = R"({"who_can_add":"admins"})";
  group.members = {{"account:alice", "owner", 1000, "c1"}, {"account:bob", "member", 2000, "c2"}};
  group.roster_statement_json = R"({"epoch":4,"sig":"abc"})";

  auto parsed = AccountStateCodec::GroupFromJson(AccountStateCodec::GroupToJson(group));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->group_id, "group:hike");
  EXPECT_EQ(parsed->title, "Weekend hike");
  EXPECT_EQ(parsed->owner_identity, "account:alice");
  EXPECT_EQ(parsed->roster_epoch, 4u);
  EXPECT_EQ(parsed->title_version, 2u);
  EXPECT_EQ(parsed->policy_json, group.policy_json);
  ASSERT_EQ(parsed->members.size(), 2u);
  EXPECT_EQ(parsed->members[0].identity, "account:alice");
  EXPECT_EQ(parsed->members[0].role, "owner");
  EXPECT_EQ(parsed->members[0].joined_at, 1000);
  EXPECT_EQ(parsed->members[0].contact_id, "c1");
  EXPECT_EQ(parsed->members[1].identity, "account:bob");
  EXPECT_EQ(parsed->members[1].role, "member");
  EXPECT_EQ(parsed->roster_statement_json, group.roster_statement_json);
}

TEST(AccountStateCodecTest, RejectsAGroupWithoutIdOrOwner) {
  AccountStateGroup group;
  group.group_id = "group:hike";
  group.owner_identity = "account:alice";
  Object no_id = AccountStateCodec::GroupToJson(group);
  no_id.set("group_id", "");
  EXPECT_FALSE(AccountStateCodec::GroupFromJson(no_id).has_value());
  Object no_owner = AccountStateCodec::GroupToJson(group);
  no_owner.set("owner", "");
  EXPECT_FALSE(AccountStateCodec::GroupFromJson(no_owner).has_value());
}

TEST(AccountStateCodecTest, DropsMembersWithoutAnIdentity) {
  AccountStateGroup group;
  group.group_id = "group:hike";
  group.owner_identity = "account:alice";
  group.members = {{"account:alice", "owner", 1000, ""}, {"", "member", 2000, "c2"}};
  auto parsed = AccountStateCodec::GroupFromJson(AccountStateCodec::GroupToJson(group));
  ASSERT_TRUE(parsed.has_value());
  ASSERT_EQ(parsed->members.size(), 1u);
  EXPECT_EQ(parsed->members[0].identity, "account:alice");
}

} // namespace
} // namespace pbr
