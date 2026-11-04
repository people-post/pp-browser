#include "feature/conversations/ContactProfileRefresh.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include "common/MeshAlias.h"

namespace pbr {
namespace {

constexpr int64_t kNow = 1'761'235'200'000;

class CountingDirectory : public mesh::IDirectoryClient {
public:
  Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string&) override {
    return std::vector<mesh::DirectoryHit>{};
  }
  Roe<mesh::DirectoryHit> LookupRelayUser(const std::string& relay_user_id) override {
    ++lookups;
    last_relay_id = relay_user_id;
    return answer;
  }
  Roe<mesh::DirectoryHit> LookupByAccount(const std::string&) override { return Error("unused"); }

  int lookups = 0;
  std::string last_relay_id;
  mesh::DirectoryHit answer;
};

class ContactProfileRefreshTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_profile_refresh_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    contacts_ = std::make_unique<ContactsStore>(dir_.string());
    refresh_ = std::make_unique<ContactProfileRefresh>(*contacts_, directory_);

    mesh::DirectoryHit hit;
    hit.account_id = "account:ada";
    hit.nickname = "Ada";
    hit.ids = {{mesh::ContactIdKind::Account, "account:ada", true},
               {mesh::ContactIdKind::RelayUser, "relay:ada", false}};
    hit.icon = mesh::ProfileIconRef{"https://cdn.example/avatars/old.png", "old", ""};
    auto added = contacts_->AddFromDirectoryHit(hit);
    ASSERT_TRUE(static_cast<bool>(added));
    id_ = added->id;
    directory_.answer = hit;
    directory_.answer.nickname = "Ada L.";
    directory_.answer.icon = mesh::ProfileIconRef{"https://cdn.example/avatars/new.png", "new", ""};
  }
  void TearDown() override {
    refresh_.reset();
    contacts_.reset();
    std::filesystem::remove_all(dir_);
  }

  /** The stored contact, its last directory answer set to `fetched_at`. */
  void SetFetchedAt(const int64_t fetched_at) {
    Contact contact = **contacts_->Get(id_);
    contact.remote.fetched_at = fetched_at;
    ASSERT_TRUE(static_cast<bool>(contacts_->Upsert(contact)));
  }

  std::filesystem::path dir_;
  CountingDirectory directory_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<ContactProfileRefresh> refresh_;
  std::string id_;
};

TEST_F(ContactProfileRefreshTest, AStaleContactTakesTheNewNicknameAndPhotoAndKeepsItsIds) {
  SetFetchedAt(kNow - ContactProfileRefresh::kStaleMs - 1);
  const Contact before = **contacts_->Get(id_);
  EXPECT_TRUE(refresh_->NeedsRefresh(id_, kNow));
  auto result = refresh_->RefreshIfStale(id_, kNow);
  EXPECT_TRUE(result.looked_up);
  EXPECT_EQ(directory_.lookups, 1);
  EXPECT_EQ(directory_.last_relay_id, "relay:ada");
  ASSERT_TRUE(result.updated.has_value());
  EXPECT_EQ(result.updated->remote.nickname, "Ada L.");
  ASSERT_TRUE(result.updated->remote.icon.has_value());
  EXPECT_EQ(result.updated->remote.icon->url, "https://cdn.example/avatars/new.png");
  EXPECT_EQ(result.updated->remote.fetched_at, kNow);
  EXPECT_EQ(result.updated->id, before.id);
  ASSERT_EQ(result.updated->remote.ids.size(), before.remote.ids.size());
  for (size_t i = 0; i < before.remote.ids.size(); ++i) {
    EXPECT_EQ(result.updated->remote.ids[i].value, before.remote.ids[i].value);
  }
  EXPECT_FALSE(refresh_->NeedsRefresh(id_, kNow)) << "just refreshed";
}

TEST_F(ContactProfileRefreshTest, AFreshContactCostsNoLookup) {
  SetFetchedAt(kNow - ContactProfileRefresh::kStaleMs + 60'000);
  EXPECT_FALSE(refresh_->NeedsRefresh(id_, kNow));
  auto result = refresh_->RefreshIfStale(id_, kNow);
  EXPECT_FALSE(result.looked_up);
  EXPECT_FALSE(result.updated.has_value());
  EXPECT_EQ(directory_.lookups, 0);
  EXPECT_EQ((**contacts_->Get(id_)).remote.nickname, "Ada");
}

TEST_F(ContactProfileRefreshTest, AnAnswerForAnotherAccountChangesNothing) {
  SetFetchedAt(kNow - ContactProfileRefresh::kStaleMs - 1);
  const Contact before = **contacts_->Get(id_);
  directory_.answer.account_id = "account:mallory";
  directory_.answer.ids = {{mesh::ContactIdKind::Account, "account:mallory", true}};
  auto result = refresh_->RefreshIfStale(id_, kNow);
  EXPECT_TRUE(result.looked_up);
  EXPECT_FALSE(result.updated.has_value());
  const Contact after = **contacts_->Get(id_);
  EXPECT_EQ(after.remote.nickname, before.remote.nickname);
  EXPECT_EQ(after.remote.icon->url, before.remote.icon->url);
  EXPECT_EQ(after.remote.fetched_at, before.remote.fetched_at);
}

TEST_F(ContactProfileRefreshTest, AnUnknownContactCostsNoLookup) {
  auto result = refresh_->RefreshIfStale("no-such-contact", kNow);
  EXPECT_FALSE(result.looked_up);
  EXPECT_EQ(directory_.lookups, 0);
}

} // namespace
} // namespace pbr
