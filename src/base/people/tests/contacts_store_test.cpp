#include "base/people/ContactsStore.h"
#include "base/people/ContactTypes.h"

#include <filesystem>
#include <gtest/gtest.h>

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

} // namespace
