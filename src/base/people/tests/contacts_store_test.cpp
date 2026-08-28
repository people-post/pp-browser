#include "base/people/ContactsStore.h"
#include "base/people/ContactTypes.h"
#include "common/ValueJson.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include "common/PbrCompat.h"

namespace {

using namespace pbr;

class ContactsStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() /
                ("pp_browser_contacts_store_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                 "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(data_dir_);
  }

  void TearDown() override { std::filesystem::remove_all(data_dir_); }

  std::filesystem::path data_dir_;
};

TEST_F(ContactsStoreTest, RemoveDeletesContactAndPersists) {
  ContactsStore store(data_dir_.string());

  Contact contact;
  contact.id = "contact-alice";
  contact.display_name = "Alice";
  contact.server_nickname = "alice";
  contact.ids = {{ContactIdKind::RelayUser, "relay:alice123", true}};
  contact.trust = TrustLevel::Unknown;

  ASSERT_TRUE(static_cast<bool>(store.Upsert(contact))) << "upsert failed";

  auto listed = store.List();
  ASSERT_TRUE(static_cast<bool>(listed));
  ASSERT_EQ(listed->size(), 1u);

  auto removed = store.Remove(contact.id);
  ASSERT_TRUE(static_cast<bool>(removed)) << removed.error().message;
  EXPECT_TRUE(*removed);

  auto missing = store.Get(contact.id);
  ASSERT_TRUE(static_cast<bool>(missing));
  EXPECT_FALSE(missing->has_value());

  auto empty_list = store.List();
  ASSERT_TRUE(static_cast<bool>(empty_list));
  EXPECT_TRUE(empty_list->empty());

  ContactsStore reloaded(data_dir_.string());
  auto reloaded_get = reloaded.Get(contact.id);
  ASSERT_TRUE(static_cast<bool>(reloaded_get));
  EXPECT_FALSE(reloaded_get->has_value());
}

TEST_F(ContactsStoreTest, RemoveMissingReturnsFalse) {
  ContactsStore store(data_dir_.string());
  auto removed = store.Remove("does-not-exist");
  ASSERT_TRUE(static_cast<bool>(removed)) << removed.error().message;
  EXPECT_FALSE(*removed);
}

TEST_F(ContactsStoreTest, AddEmptyPersists) {
  ContactsStore store(data_dir_.string());

  auto created = store.AddEmpty();
  ASSERT_TRUE(static_cast<bool>(created)) << "add empty failed";
  EXPECT_FALSE(created->id.empty());
  EXPECT_TRUE(created->display_name.empty());
  EXPECT_TRUE(created->server_nickname.empty());
  EXPECT_TRUE(created->ids.empty());
  EXPECT_TRUE(created->multiaddrs.empty());

  ContactsStore reloaded(data_dir_.string());
  auto loaded = reloaded.Get(created->id);
  ASSERT_TRUE(static_cast<bool>(loaded));
  ASSERT_TRUE(loaded->has_value());
  EXPECT_EQ(loaded->value().id, created->id);
  EXPECT_TRUE(loaded->value().display_name.empty());
}

TEST_F(ContactsStoreTest, AddFromDirectoryHitMergesByAccountAndPreservesLocal) {
  ContactsStore store(data_dir_.string());

  DirectoryHit hit;
  hit.hit_id = "relay:bob";
  hit.display_name = "Bob";
  hit.nickname = "bob";
  hit.account_id = "account:bob";
  hit.ids = {{ContactIdKind::Account, "account:bob", true},
             {ContactIdKind::RelayUser, "relay:bob", false}};
  hit.multiaddrs = {"/ip4/127.0.0.1/tcp/4001/p2p/12D3KooWBob"};

  auto first = store.AddFromDirectoryHit(hit);
  ASSERT_TRUE(static_cast<bool>(first)) << first.error().message;
  Contact edited = *first;
  edited.local.display_name = "Bobby";
  edited.local.trust = TrustLevel::Friendly;
  SyncContactMirrors(edited);
  ASSERT_TRUE(static_cast<bool>(store.Upsert(edited)));

  hit.nickname = "robert";
  hit.multiaddrs = {"/ip4/10.0.0.1/tcp/4001/p2p/12D3KooWBob"};
  auto second = store.AddFromDirectoryHit(hit);
  ASSERT_TRUE(static_cast<bool>(second)) << second.error().message;
  EXPECT_EQ(second->id, first->id);
  EXPECT_EQ(second->local.display_name, "Bobby");
  EXPECT_EQ(second->local.trust, TrustLevel::Friendly);
  EXPECT_EQ(second->remote.nickname, "robert");
  ASSERT_EQ(second->remote.multiaddrs.size(), 1u);
  EXPECT_EQ(second->remote.multiaddrs[0], "/ip4/10.0.0.1/tcp/4001/p2p/12D3KooWBob");
  EXPECT_GT(second->remote.fetched_at, 0);
}

TEST_F(ContactsStoreTest, AddFromDirectoryHitRequiresAccountId) {
  ContactsStore store(data_dir_.string());
  DirectoryHit hit;
  hit.hit_id = "relay:bob";
  hit.nickname = "bob";
  hit.ids = {{ContactIdKind::RelayUser, "relay:bob", true}};
  auto result = store.AddFromDirectoryHit(hit);
  ASSERT_FALSE(static_cast<bool>(result));
  EXPECT_NE(result.error().message.find("Account ID"), std::string::npos);
}

TEST_F(ContactsStoreTest, SchemaVersionWrittenAndLegacyMigrated) {
  const auto path = data_dir_ / "contacts.json";
  std::filesystem::create_directories(data_dir_);
  {
    Object id_row;
    id_row.set("kind", "relay_user");
    id_row.set("value", "relay:alice");
    id_row.set("primary", true);

    Object contact;
    contact.set("id", "c1");
    contact.set("display_name", "Alice");
    contact.set("server_nickname", "alice");
    contact.set("ids", ArrayValue({ObjectValue(std::move(id_row))}));
    contact.set("multiaddrs", ArrayValue({}));
    contact.set("trust", "friendly");

    Object legacy;
    legacy.set("contacts", ArrayValue({ObjectValue(std::move(contact))}));
    std::ofstream out(path);
    ASSERT_TRUE(static_cast<bool>(out));
    out << DumpJson(legacy, 2);
  }

  ContactsStore store(data_dir_.string());
  auto listed = store.List();
  ASSERT_TRUE(static_cast<bool>(listed)) << listed.error().message;
  ASSERT_EQ(listed->size(), 1u);
  EXPECT_EQ(listed->front().local.display_name, "Alice");
  EXPECT_EQ(listed->front().remote.nickname, "alice");
  EXPECT_EQ(listed->front().local.trust, TrustLevel::Friendly);

  std::ifstream in(path);
  ASSERT_TRUE(static_cast<bool>(in));
  std::ostringstream ss;
  ss << in.rdbuf();
  auto rewritten = TryParseObject(ss.str());
  ASSERT_TRUE(static_cast<bool>(rewritten));
  ASSERT_TRUE(rewritten->contains("schema_version"));
  EXPECT_EQ(static_cast<int>(rewritten->getIf<int64_t>("schema_version").value_or(-1)),
            ContactsStore::kSchemaVersion);
  const Array* contacts = rewritten->getArray("contacts");
  ASSERT_NE(contacts, nullptr);
  ASSERT_EQ(contacts->elements.size(), 1u);
  const Object* first = asObject(contacts->elements[0]);
  ASSERT_NE(first, nullptr);
  EXPECT_TRUE(first->contains("local"));
  EXPECT_TRUE(first->contains("remote"));
  EXPECT_TRUE(first->contains("overrides"));
}

TEST_F(ContactsStoreTest, RejectsNewerSchemaVersion) {
  const auto path = data_dir_ / "contacts.json";
  std::filesystem::create_directories(data_dir_);
  {
    Object newer;
    newer.set("schema_version", static_cast<int64_t>(ContactsStore::kSchemaVersion + 1));
    newer.set("contacts", ArrayValue({}));
    std::ofstream out(path);
    ASSERT_TRUE(static_cast<bool>(out));
    out << DumpJson(newer, 2);
  }

  ContactsStore store(data_dir_.string());
  auto listed = store.List();
  ASSERT_FALSE(static_cast<bool>(listed));
  EXPECT_NE(listed.error().message.find("schema"), std::string::npos);
}

} // namespace
