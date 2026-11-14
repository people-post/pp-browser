#include "feature/conversations/ContactProfileRefresh.h"

#include "lib/people/ContactIdentity.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <functional>
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
  Roe<mesh::DirectoryHit> LookupByAccount(const std::string& account_id) override {
    ++lookups;
    last_account_id = account_id;
    if (on_account_lookup) {
      on_account_lookup();
    }
    if (fail) {
      return Error("not found");
    }
    return answer;
  }

  int lookups = 0;
  bool fail = false;
  std::string last_account_id;
  mesh::DirectoryHit answer;
  std::function<void()> on_account_lookup; // runs inside the lookup (an overlapping call)
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
    hit.ids = {{mesh::ContactIdKind::Account, "account:ada", true}};
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
  EXPECT_EQ(directory_.last_account_id, "account:ada") << "by Account ID (MR001)";
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

TEST_F(ContactProfileRefreshTest, AContactWithoutAnAccountIdIsSkipped) {
  Contact contact;
  contact.id = "no-ids";
  contact.remote.fetched_at = 1;
  ASSERT_TRUE(static_cast<bool>(contacts_->Upsert(contact)));
  EXPECT_FALSE(refresh_->NeedsRefresh("no-ids", kNow));
  EXPECT_FALSE(refresh_->RefreshIfStale("no-ids", kNow).looked_up);
  EXPECT_EQ(directory_.lookups, 0);
}

class ScannedContactRefreshTest : public ContactProfileRefreshTest {
protected:
  void SetUp() override {
    ContactProfileRefreshTest::SetUp();
    // Added from a QR code: Account ID and a device, a fetch time of "just now".
    Contact contact;
    contact.id = "scanned";
    contact.remote.ids = {{mesh::ContactIdKind::Account, "account:bob", true},
                          {mesh::ContactIdKind::PeerId, "12D3KooWBob", false}};
    contact.remote.nickname = "Bob";
    contact.remote.fetched_at = kNow;
    ASSERT_TRUE(static_cast<bool>(contacts_->Upsert(contact)));
    directory_.answer = mesh::DirectoryHit{};
    directory_.answer.account_id = "account:bob";
    directory_.answer.nickname = "Bob B.";
    directory_.answer.ids = {{mesh::ContactIdKind::Account, "account:bob", true}};
  }
  void SetScannedFetchedAt(const int64_t fetched_at) {
    Contact contact = **contacts_->Get("scanned");
    contact.remote.fetched_at = fetched_at;
    ASSERT_TRUE(static_cast<bool>(contacts_->Upsert(contact)));
  }
};

TEST_F(ScannedContactRefreshTest, AFreshScannedContactIsNotDueByItselfButAForcedCallLooksItUp) {
  EXPECT_FALSE(refresh_->NeedsRefresh("scanned", kNow)) << "fetched just now: not stale";
  EXPECT_FALSE(refresh_->RefreshIfStale("scanned", kNow).looked_up);
  EXPECT_EQ(directory_.lookups, 0);
  EXPECT_TRUE(refresh_->NeedsRefresh("scanned", kNow, /*force=*/true));
  auto result = refresh_->RefreshIfStale("scanned", kNow, /*force=*/true);
  EXPECT_TRUE(result.looked_up);
  EXPECT_EQ(directory_.lookups, 1);
  ASSERT_TRUE(result.updated.has_value());
  EXPECT_EQ(result.updated->remote.nickname, "Bob B.");
  EXPECT_FALSE(refresh_->NeedsRefresh("scanned", kNow + 1, true)) << "not asked again within kStaleMs";
}

TEST_F(ScannedContactRefreshTest, TheAutomaticPathTakesNoneOfTheAnswersDevices) {
  directory_.answer.ids.push_back({mesh::ContactIdKind::PeerId, "12D3KooWOther", false});
  directory_.answer.endpoints = {mesh::DirectoryEndpoint{.peer_id = "12D3KooWOther"}};
  directory_.answer.multiaddrs = {"/ip4/9.9.9.9/tcp/9"};
  const Contact before = **contacts_->Get("scanned");
  auto result = refresh_->RefreshIfStale("scanned", kNow, true);
  ASSERT_TRUE(result.updated.has_value());
  EXPECT_EQ(PrimaryIdOfKind(*result.updated, mesh::ContactIdKind::PeerId), "12D3KooWBob");
  for (const mesh::ContactId& id : result.updated->remote.ids) {
    EXPECT_NE(id.value, "12D3KooWOther");
  }
  EXPECT_EQ(result.updated->remote.endpoints.size(), before.remote.endpoints.size());
  EXPECT_EQ(result.updated->remote.multiaddrs, before.remote.multiaddrs);
}

TEST_F(ScannedContactRefreshTest, AStaleAnswerUpdatesTheNicknameAndTheFetchTime) {
  SetScannedFetchedAt(kNow - ContactProfileRefresh::kStaleMs - 1);
  auto result = refresh_->RefreshIfStale("scanned", kNow);
  ASSERT_TRUE(result.updated.has_value());
  EXPECT_EQ(result.updated->remote.nickname, "Bob B.");
  EXPECT_EQ(result.updated->remote.fetched_at, kNow);
  EXPECT_FALSE(refresh_->NeedsRefresh("scanned", kNow + 1));
  EXPECT_FALSE(refresh_->NeedsRefresh("scanned", kNow + ContactProfileRefresh::kStaleMs - 1));
  EXPECT_TRUE(refresh_->NeedsRefresh("scanned", kNow + ContactProfileRefresh::kStaleMs));
}

TEST_F(ScannedContactRefreshTest, OverlappingCallsDoOneLookup) {
  ContactProfileRefresh::Result inner;
  directory_.on_account_lookup = [&] { inner = refresh_->RefreshIfStale("scanned", kNow, true); };
  auto outer = refresh_->RefreshIfStale("scanned", kNow, true);
  EXPECT_TRUE(outer.looked_up);
  EXPECT_FALSE(inner.looked_up);
  EXPECT_EQ(directory_.lookups, 1);
}

TEST_F(ScannedContactRefreshTest, AFailedLookupIsNotRepeatedWithinSixHours) {
  SetScannedFetchedAt(kNow - ContactProfileRefresh::kStaleMs - 1);
  directory_.fail = true;
  EXPECT_TRUE(refresh_->RefreshIfStale("scanned", kNow).looked_up);
  EXPECT_FALSE(refresh_->NeedsRefresh("scanned", kNow + 1000));
  EXPECT_FALSE(refresh_->RefreshIfStale("scanned", kNow + 1000).looked_up);
  EXPECT_EQ(directory_.lookups, 1);
  EXPECT_TRUE(refresh_->NeedsRefresh("scanned", kNow + ContactProfileRefresh::kStaleMs));
  EXPECT_TRUE(refresh_->RefreshIfStale("scanned", kNow + ContactProfileRefresh::kStaleMs).looked_up);
  EXPECT_EQ(directory_.lookups, 2);
}

TEST_F(ScannedContactRefreshTest, AnAnswerForAnotherAccountFillsNothingIn) {
  directory_.answer.account_id = "account:mallory";
  directory_.answer.ids = {{mesh::ContactIdKind::Account, "account:mallory", true}};
  auto result = refresh_->RefreshIfStale("scanned", kNow, true);
  EXPECT_TRUE(result.looked_up);
  EXPECT_FALSE(result.updated.has_value());
  EXPECT_EQ((*contacts_->Get("scanned"))->remote.nickname, "Bob");
}

} // namespace
} // namespace pbr
