#include "common/chat/PeopleDiscoveryBlocks.h"

#include <gtest/gtest.h>
#include "common/ValueJson.h"

namespace {

using namespace pbr;

TEST(PeopleDiscoveryBlocksTest, DirectoryHitUsesIdentityAndPrimaryAdd) {
  DirectoryHit hit;
  hit.hit_id = "hit_alice";
  hit.display_name = "Alice Example";
  hit.nickname = "alice";
  hit.ids = {{ContactIdKind::RelayUser, "relay:alice123456789", true},
             {ContactIdKind::Account, "account:alice123", false}};

  const std::string blocks = BuildPeopleDiscoveryBlocksJson({hit}, {});
  EXPECT_NE(blocks.find("Alice Example"), std::string::npos);
  EXPECT_NE(blocks.find("~alice"), std::string::npos);
  EXPECT_NE(blocks.find("@account:alice123"), std::string::npos);
  EXPECT_NE(blocks.find("avatar_letter"), std::string::npos);
  EXPECT_NE(blocks.find("\"style\":\"primary\""), std::string::npos);
  EXPECT_NE(blocks.find("Add contact"), std::string::npos);
  EXPECT_EQ(blocks.find("In contacts"), std::string::npos);

  auto parsed = ParseValue(blocks);
  ASSERT_TRUE(static_cast<bool>(parsed));
  const Object* root = asObject(*parsed);
  ASSERT_NE(root, nullptr);
  const Array* block_list = root->getArray("blocks");
  ASSERT_NE(block_list, nullptr);
  ASSERT_GE(block_list->elements.size(), 2u);
  const Object* list = asObject(block_list->elements[1]);
  ASSERT_NE(list, nullptr);
  const Array* items = list->getArray("items");
  ASSERT_NE(items, nullptr);
  ASSERT_EQ(items->elements.size(), 1u);
  const Object* item = asObject(items->elements[0]);
  ASSERT_NE(item, nullptr);
  EXPECT_EQ(item->getString("title").value_or(""), "Alice Example");
  EXPECT_EQ(item->getString("subtitle").value_or(""), "~alice · @account:alice123");
  EXPECT_FALSE(item->getString("meta").has_value());
}

TEST(PeopleDiscoveryBlocksTest, AlreadyContactPrefersMessageAndMeta) {
  DirectoryHit hit;
  hit.hit_id = "hit_bob";
  hit.display_name = "Bob";
  hit.nickname = "bob";
  hit.ids = {{ContactIdKind::RelayUser, "relay:bob1", true}};

  PeopleDiscoveryContactView local;
  local.id = "c_bob";
  local.display_name = "Bob";
  local.server_nickname = "bob";
  local.ids = {{ContactIdKind::RelayUser, "relay:bob1", true}};

  PeopleDiscoveryBuildOptions options;
  options.known_local_identity_values.insert("relay:bob1");

  const std::string blocks = BuildPeopleDiscoveryBlocksJson({hit}, {local}, options);
  EXPECT_NE(blocks.find("In contacts"), std::string::npos);
  EXPECT_NE(blocks.find("\"type\":\"start_conversation\""), std::string::npos);
  EXPECT_NE(blocks.find("\"contact_id\":\"c_bob\""), std::string::npos);
  EXPECT_NE(blocks.find("\"label\":\"Message\""), std::string::npos);
  EXPECT_EQ(blocks.find("Add contact"), std::string::npos);
}

TEST(PeopleDiscoveryBlocksTest, CapsLongResultsWithRefineFooter) {
  std::vector<DirectoryHit> hits;
  for (int i = 0; i < 15; ++i) {
    DirectoryHit hit;
    hit.hit_id = "hit_" + std::to_string(i);
    hit.display_name = "Person " + std::to_string(i);
    hit.nickname = "p" + std::to_string(i);
    hit.ids = {{ContactIdKind::RelayUser, "relay:p" + std::to_string(i), true}};
    hits.push_back(std::move(hit));
  }

  PeopleDiscoveryBuildOptions options;
  options.max_visible_items = 10;
  const std::string blocks = BuildPeopleDiscoveryBlocksJson(hits, {}, options);
  EXPECT_NE(blocks.find("Found 15 people"), std::string::npos);
  EXPECT_NE(blocks.find("Refine search"), std::string::npos);
  EXPECT_NE(blocks.find("Person 0"), std::string::npos);
  EXPECT_NE(blocks.find("Person 9"), std::string::npos);
  EXPECT_EQ(blocks.find("Person 10"), std::string::npos);
}

} // namespace
