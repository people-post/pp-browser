#include "common/chat/PeopleDiscoveryBlocks.h"

#include <gtest/gtest.h>
#include "common/chat/MessagingLimits.h"
#include "common/ValueJson.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace {

using namespace pbr;

TEST(PeopleDiscoveryBlocksTest, NoMatchesExplainsTheMissWithoutOfferingToAddAnyone) {
  const std::string blocks = BuildPeopleDiscoveryBlocksJson({}, {});
  EXPECT_NE(blocks.find("No people found"), std::string::npos);
  EXPECT_EQ(blocks.find("\"type\":\"add_contact\""), std::string::npos);
}

TEST(PeopleDiscoveryBlocksTest, DirectoryHitUsesIdentityAndPrimaryAdd) {
  mesh::DirectoryHit hit;
  hit.hit_id = "hit_alice";
  hit.display_name = "Alice Example";
  hit.nickname = "alice";
  hit.ids = {{mesh::ContactIdKind::RelayUser, "relay:alice123456789", true},
             {mesh::ContactIdKind::Account, "account:alice123", false}};

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

TEST(PeopleDiscoveryBlocksTest, RowActionsNameThePersonOnly) {
  mesh::DirectoryHit hit;
  hit.hit_id = "hit_alice";
  hit.display_name = "Alice Example";
  hit.nickname = "alice";
  hit.account_id = "account:alice123";
  hit.ids = {{mesh::ContactIdKind::Account, "account:alice123", true}};
  hit.signing_public_key_b64 = "SIGNKEY";
  hit.kem_public_key_b64 = "KEMKEY";
  hit.endpoints.push_back(mesh::DirectoryEndpoint{.peer_id = "QmPeer"});

  const Object list = [&] {
    auto doc = TryParseObject(BuildPeopleDiscoveryBlocksJson({hit}, {}));
    const Array* blocks = doc ? doc->getArray("blocks") : nullptr;
    const Object* block = blocks && blocks->elements.size() > 1 ? asObject(blocks->elements[1]) : nullptr;
    return block ? *block : Object{};
  }();
  const Array* items = list.getArray("items");
  ASSERT_NE(items, nullptr);
  ASSERT_EQ(items->elements.size(), 1u);
  const Array* actions = asObject(items->elements[0])->getArray("actions");
  ASSERT_NE(actions, nullptr);
  ASSERT_EQ(actions->elements.size(), 2u);
  const char* const types[] = {"add_contact", "start_conversation"};
  for (size_t i = 0; i < 2; ++i) {
    const Object* payload = asObject(actions->elements[i])->getObject("payload");
    ASSERT_NE(payload, nullptr);
    EXPECT_EQ(payload->getString("type").value_or(""), types[i]);
    EXPECT_EQ(payload->getString("account_id").value_or(""), "account:alice123");
    EXPECT_EQ(payload->getString("hit_id").value_or(""), "hit_alice");
    EXPECT_EQ(payload->getString("nickname").value_or(""), "alice");
    EXPECT_EQ(payload->getString("display_name").value_or(""), "Alice Example");
    EXPECT_FALSE(payload->contains("directory_hit"));
    const std::string dumped = DumpJson(*payload);
    EXPECT_EQ(dumped.find("SIGNKEY"), std::string::npos) << dumped;
    EXPECT_EQ(dumped.find("KEMKEY"), std::string::npos) << dumped;
    EXPECT_EQ(dumped.find("QmPeer"), std::string::npos) << dumped;
  }
}

TEST(PeopleDiscoveryBlocksTest, AlreadyContactPrefersMessageAndMeta) {
  mesh::DirectoryHit hit;
  hit.hit_id = "hit_bob";
  hit.display_name = "Bob";
  hit.nickname = "bob";
  hit.ids = {{mesh::ContactIdKind::RelayUser, "relay:bob1", true}};

  PeopleDiscoveryContactView local;
  local.id = "c_bob";
  local.display_name = "Bob";
  local.server_nickname = "bob";
  local.ids = {{mesh::ContactIdKind::RelayUser, "relay:bob1", true}};

  PeopleDiscoveryBuildOptions options;
  options.known_local_identity_values.insert("relay:bob1");

  const std::string blocks = BuildPeopleDiscoveryBlocksJson({hit}, {local}, options);
  EXPECT_NE(blocks.find("In contacts"), std::string::npos);
  EXPECT_NE(blocks.find("\"type\":\"start_conversation\""), std::string::npos);
  EXPECT_NE(blocks.find("\"contact_id\":\"c_bob\""), std::string::npos);
  EXPECT_NE(blocks.find("\"label\":\"Message\""), std::string::npos);
  EXPECT_EQ(blocks.find("Add contact"), std::string::npos);
}

TEST(PeopleDiscoveryBlocksTest, ContactRowShowsRemarkWithNicknameSmallOrJustTheNickname) {
  PeopleDiscoveryContactView remarked;
  remarked.id = "c1";
  remarked.display_name = "Alice-work";
  remarked.server_nickname = "Alice";
  remarked.ids = {{mesh::ContactIdKind::Account, "account:alice1", true}};
  PeopleDiscoveryContactView plain = remarked;
  plain.id = "c2";
  plain.display_name = "";

  auto first_item = [](const std::string& blocks_json) {
    auto root = TryParseObject(blocks_json);
    const Array* blocks = root->getArray("blocks");
    const Object* list = asObject(blocks->elements.back());
    return *asObject(list->getArray("items")->elements[0]);
  };
  const Object with_remark = first_item(BuildPeopleDiscoveryBlocksJson({}, {remarked}));
  EXPECT_EQ(with_remark.getString("title").value_or(""), "Alice-work");
  EXPECT_NE(with_remark.getString("subtitle").value_or("").find("~Alice"), std::string::npos);

  const Object without = first_item(BuildPeopleDiscoveryBlocksJson({}, {plain}));
  EXPECT_EQ(without.getString("title").value_or(""), "Alice");
  EXPECT_EQ(without.getString("subtitle").value_or("").find("~Alice"), std::string::npos);
}

std::vector<mesh::DirectoryHit> NumberedHits(const int count) {
  std::vector<mesh::DirectoryHit> hits;
  for (int i = 0; i < count; ++i) {
    mesh::DirectoryHit hit;
    hit.hit_id = "hit_" + std::to_string(i);
    hit.display_name = "Person " + std::to_string(i);
    hit.nickname = "p" + std::to_string(i);
    hit.ids = {{mesh::ContactIdKind::RelayUser, "relay:p" + std::to_string(i), true}};
    hits.push_back(std::move(hit));
  }
  return hits;
}

Object ListBlock(const std::string& blocks_json) {
  auto doc = TryParseObject(blocks_json);
  EXPECT_TRUE(doc.has_value()) << blocks_json;
  const Array* blocks = doc ? doc->getArray("blocks") : nullptr;
  EXPECT_NE(blocks, nullptr);
  EXPECT_EQ(blocks ? blocks->elements.size() : 0u, 2u);
  const Object* list = blocks && blocks->elements.size() > 1 ? asObject(blocks->elements[1]) : nullptr;
  EXPECT_NE(list, nullptr);
  return list ? *list : Object{};
}

TEST(PeopleDiscoveryBlocksTest, LongResultsCarryEveryRowAndShowMoreInPlace) {
  PeopleDiscoveryBuildOptions options;
  options.max_visible_items = 10;
  const std::string blocks = BuildPeopleDiscoveryBlocksJson(NumberedHits(17), {}, options);
  EXPECT_NE(blocks.find("Found 17 people on the network:"), std::string::npos) << blocks;

  const Object list = ListBlock(blocks);
  EXPECT_EQ(list.getString("title").value_or(""), "Search results");
  ASSERT_NE(list.getArray("items"), nullptr);
  EXPECT_EQ(list.getArray("items")->elements.size(), 17u);
  EXPECT_EQ(list.getNonNegInt("visible").value_or(0), 10u);

  const Array* footer = list.getArray("footer_actions");
  ASSERT_NE(footer, nullptr);
  ASSERT_EQ(footer->elements.size(), 1u) << "show more only, no refine next to it";
  const Object* more = asObject(footer->elements[0]);
  ASSERT_NE(more, nullptr);
  EXPECT_EQ(more->getString("label").value_or(""), "Show more (7 left)");
  const Object* payload = more->getObject("payload");
  ASSERT_NE(payload, nullptr);
  EXPECT_EQ(payload->getString("type").value_or(""), kShowMoreRowsActionType);
  EXPECT_EQ(payload->getNonNegInt("block").value_or(99), 1u);
  EXPECT_EQ(payload->getNonNegInt("step").value_or(0), 10u);
  EXPECT_FALSE(payload->contains("tool")) << "local action, not a new turn";
  EXPECT_EQ(blocks.find("Refine search"), std::string::npos);
}

TEST(PeopleDiscoveryBlocksTest, RevealMoreRowsGrowsVisibleThenDropsTheFooter) {
  PeopleDiscoveryBuildOptions options;
  options.max_visible_items = 10;
  const std::string blocks = BuildPeopleDiscoveryBlocksJson(NumberedHits(27), {}, options);

  auto step1 = RevealMoreRowsInBlocksJson(blocks, 1, 10);
  ASSERT_TRUE(step1.has_value());
  Object list = ListBlock(*step1);
  EXPECT_EQ(list.getNonNegInt("visible").value_or(0), 20u);
  ASSERT_NE(list.getArray("footer_actions"), nullptr);
  ASSERT_EQ(list.getArray("footer_actions")->elements.size(), 1u);
  const Object* more = asObject(list.getArray("footer_actions")->elements[0]);
  ASSERT_NE(more, nullptr);
  EXPECT_EQ(more->getString("label").value_or(""), "Show more (7 left)");
  ASSERT_NE(more->getObject("payload"), nullptr);
  EXPECT_EQ(more->getObject("payload")->getNonNegInt("block").value_or(99), 1u);

  auto step2 = RevealMoreRowsInBlocksJson(*step1, 1, 10);
  ASSERT_TRUE(step2.has_value());
  list = ListBlock(*step2);
  EXPECT_EQ(list.getNonNegInt("visible").value_or(0), 27u);
  EXPECT_FALSE(list.contains("footer_actions")) << "everything shows: no footer";

  EXPECT_FALSE(RevealMoreRowsInBlocksJson(*step2, 1, 10).has_value()) << "nothing left to reveal";
  EXPECT_FALSE(RevealMoreRowsInBlocksJson(blocks, 0, 10).has_value()) << "block 0 is the paragraph";
  EXPECT_FALSE(RevealMoreRowsInBlocksJson(blocks, 5, 10).has_value()) << "no such block";
}

TEST(PeopleDiscoveryBlocksTest, RevealMoreRowsOnSeventeenHits) {
  PeopleDiscoveryBuildOptions options;
  options.max_visible_items = 10;
  Object list = ListBlock(BuildPeopleDiscoveryBlocksJson(NumberedHits(17), {}, options));
  EXPECT_TRUE(RevealMoreRows(list, 10));
  EXPECT_EQ(list.getNonNegInt("visible").value_or(0), 17u);
  EXPECT_FALSE(list.contains("footer_actions"));
  EXPECT_FALSE(RevealMoreRows(list, 10));
}

TEST(PeopleDiscoveryBlocksTest, ShortResultsHaveNoFooter) {
  const std::string blocks = BuildPeopleDiscoveryBlocksJson(NumberedHits(5), {});
  const Object list = ListBlock(blocks);
  ASSERT_NE(list.getArray("items"), nullptr);
  EXPECT_EQ(list.getArray("items")->elements.size(), 5u);
  EXPECT_EQ(list.getNonNegInt("visible").value_or(0), 5u);
  EXPECT_FALSE(list.contains("footer_actions"));
  EXPECT_EQ(blocks.find("Show more"), std::string::npos);
  EXPECT_EQ(blocks.find("Refine search"), std::string::npos);
}

TEST(PeopleDiscoveryBlocksTest, DropsSelfIdentityHits) {
  mesh::DirectoryHit self_hit;
  self_hit.hit_id = "hit_me";
  self_hit.display_name = "Me";
  self_hit.nickname = "me";
  self_hit.account_id = "account:me";
  self_hit.ids = {{mesh::ContactIdKind::Account, "account:me", true},
                  {mesh::ContactIdKind::RelayUser, "relay:me", false}};

  mesh::DirectoryHit other;
  other.hit_id = "hit_other";
  other.display_name = "Other";
  other.nickname = "other";
  other.ids = {{mesh::ContactIdKind::RelayUser, "relay:other", true}};

  PeopleDiscoveryBuildOptions options;
  options.self_identity_values = {"account:me", "relay:me"};

  const std::string blocks = BuildPeopleDiscoveryBlocksJson({self_hit, other}, {}, options);
  EXPECT_EQ(blocks.find("\"title\":\"Me\""), std::string::npos);
  EXPECT_NE(blocks.find("Other"), std::string::npos);
  EXPECT_NE(blocks.find("Found 1 person"), std::string::npos);
}

TEST(PeopleDiscoveryBlocksTest, NestedContactJsonAnnotatesAlreadyKnown) {
  mesh::DirectoryHit hit;
  hit.hit_id = "hit_nested";
  hit.display_name = "Nested";
  hit.nickname = "nested";
  hit.ids = {{mesh::ContactIdKind::RelayUser, "relay:nested1", true}};

  const std::string contacts_json = R"([
    {
      "id": "c_nested",
      "local": { "display_name": "Nested", "trust": "friendly" },
      "remote": {
        "nickname": "nested",
        "ids": [{ "kind": "relay_user", "value": "relay:nested1", "primary": true }]
      },
      "overrides": {}
    }
  ])";
  const std::string blocks = TryPeopleDiscoveryBlocksFromToolJson(contacts_json);
  // list_contacts-shaped JSON alone builds a contacts long_list
  EXPECT_NE(blocks.find("Nested"), std::string::npos);
  EXPECT_NE(blocks.find("Message"), std::string::npos);

  PeopleDiscoveryBuildOptions options;
  options.known_local_identity_values.insert("relay:nested1");
  PeopleDiscoveryContactView local;
  local.id = "c_nested";
  local.display_name = "Nested";
  local.ids = {{mesh::ContactIdKind::RelayUser, "relay:nested1", true}};
  const std::string annotated = BuildPeopleDiscoveryBlocksJson({hit}, {local}, options);
  EXPECT_NE(annotated.find("In contacts"), std::string::npos);
  EXPECT_EQ(annotated.find("Add contact"), std::string::npos);
}

} // namespace

// Size probe (2026-10-24): a directory hit carries ML-DSA-65 / ML-KEM-768 keys and a binding signature; the
// list stores every hit in the message text, which the thread store caps at kMaxComposeTextBytes. Row actions
// therefore name the person only; the keys are looked up when Add / Message is used.
TEST(PeopleDiscoveryBlocksTest, SeventeenKeyedHitsFitTheStoredMessage) {
  std::vector<mesh::DirectoryHit> hits;
  for (int i = 0; i < 17; ++i) {
    mesh::DirectoryHit hit;
    hit.hit_id = "hit_" + std::to_string(i);
    hit.display_name = "Person " + std::to_string(i);
    hit.nickname = "person" + std::to_string(i);
    hit.account_id = "account:" + std::string(43, 'A' + (i % 26));
    hit.ids = {{mesh::ContactIdKind::Account, *hit.account_id, true},
               {mesh::ContactIdKind::RelayUser, "relay:" + std::string(16, 'r'), false}};
    hit.signing_public_key_b64 = std::string(2604, 'S');  // ML-DSA-65 public key, base64
    hit.kem_public_key_b64 = std::string(1580, 'K');      // ML-KEM-768 public key, base64
    hit.kem_binding_signature_b64 = std::string(4412, 'B');
    hit.endpoints.push_back(mesh::DirectoryEndpoint{.peer_id = "Qm" + std::string(44, 'p')});
    hits.push_back(hit);
  }
  const std::string json = pbr::BuildPeopleDiscoveryBlocksJson(hits, {}, pbr::PeopleDiscoveryBuildOptions{});
  EXPECT_LE(json.size(), pbr::kMaxComposeTextBytes) << "list json bytes=" << json.size();
  EXPECT_EQ(json.find("signing_public_key_b64"), std::string::npos);
  EXPECT_EQ(json.find("directory_hit"), std::string::npos);
}

// A result set over the list cap is cut and says so (a refine action), never past kMaxComposeTextBytes.
TEST(PeopleDiscoveryBlocksTest, ThreeHundredKeyedHitsStayUnderTheStoreLimitAndOfferRefine) {
  std::vector<mesh::DirectoryHit> hits;
  for (int i = 0; i < 300; ++i) {
    mesh::DirectoryHit hit;
    hit.hit_id = "hit_" + std::to_string(i);
    hit.display_name = "Person " + std::to_string(i);
    hit.nickname = "person" + std::to_string(i);
    hit.account_id = "account:" + std::string(43, 'A' + (i % 26));
    hit.ids = {{mesh::ContactIdKind::Account, *hit.account_id, true},
               {mesh::ContactIdKind::RelayUser, "relay:" + std::string(16, 'r'), false}};
    hit.signing_public_key_b64 = std::string(2604, 'S');
    hit.kem_public_key_b64 = std::string(1580, 'K');
    hit.kem_binding_signature_b64 = std::string(4412, 'B');
    hits.push_back(hit);
  }
  const pbr::PeopleDiscoveryBuildOptions options;
  const std::string json = pbr::BuildPeopleDiscoveryBlocksJson(hits, {}, options);
  EXPECT_LE(json.size(), pbr::kMaxComposeTextBytes) << "list json bytes=" << json.size();
  EXPECT_NE(json.find(options.labels.refine_label), std::string::npos);
  EXPECT_NE(json.find("Show more (90 left)"), std::string::npos);
  EXPECT_EQ(json.find("hit_100\""), std::string::npos);

  // Revealing every cut-off-free row leaves the refine action, drops show-more.
  std::string current = json;
  for (int i = 0; i < 9; ++i) {
    auto next = pbr::RevealMoreRowsInBlocksJson(current, 1, 10, options.labels);
    ASSERT_TRUE(next.has_value());
    current = *next;
  }
  EXPECT_EQ(current.find("show_more_rows"), std::string::npos);
  EXPECT_NE(current.find(options.labels.refine_label), std::string::npos);
}
