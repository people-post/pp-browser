#include "feature/conversations/OwnProfileIconSync.h"

#include "lib/people/ProfileIconCache.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "common/MeshAlias.h"

namespace pbr {
namespace {

constexpr int64_t kNow = 1'761'235'200'000;
const char kAccount[] = "account:me";

class FakeDirectory : public mesh::IDirectoryClient {
public:
  Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string&) override {
    return std::vector<mesh::DirectoryHit>{};
  }
  Roe<mesh::DirectoryHit> LookupByAccount(const std::string& account_id) override {
    ++lookups;
    last_account_id = account_id;
    if (fail) {
      return Error("directory down");
    }
    return answer;
  }

  int lookups = 0;
  bool fail = false;
  std::string last_account_id;
  mesh::DirectoryHit answer;
};

class OwnProfileIconSyncTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_own_icon_sync_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    directory_.answer.account_id = kAccount;
    sync_ = std::make_unique<OwnProfileIconSync>(
        dir_.string(), directory_, [this]() { return account_; },
        [this](const std::string& profile_dir, const std::string& key, const mesh::ProfileIconRef& icon) -> Roe<void> {
          ++fetches;
          fetched_urls.push_back(icon.url);
          if (fetch_fails) {
            return Error("download failed");
          }
          // The fake icon client: the bytes of a photo, cached the way the real fetch does.
          ProfileIconCacheMeta meta;
          meta.url = icon.url;
          meta.blob_id = icon.blob_id;
          meta.kind = icon.kind;
          meta.fetched_at_ms = now_;
          return SaveProfileIconCache(profile_dir, {1, 2, 3}, "png", meta, key);
        },
        [this]() { return now_; });
  }
  void TearDown() override {
    sync_.reset();
    std::filesystem::remove_all(dir_);
  }

  void Advance(const int64_t ms) { now_ += ms; }

  std::filesystem::path dir_;
  FakeDirectory directory_;
  std::unique_ptr<OwnProfileIconSync> sync_;
  std::string account_ = kAccount;
  int64_t now_ = kNow;
  int fetches = 0;
  bool fetch_fails = false;
  std::vector<std::string> fetched_urls;
};

TEST_F(OwnProfileIconSyncTest, ALinkedDeviceWithNoPhotoFetchesTheOneTheDirectoryShows) {
  directory_.answer.icon = mesh::ProfileIconRef{"https://cdn.example/me-1.png", "b1", "png"};
  EXPECT_TRUE(ProfileIconLocalPath(dir_.string()).empty());
  EXPECT_EQ(sync_->Refresh(), OwnProfileIconSync::Outcome::Fetched);
  EXPECT_EQ(directory_.last_account_id, kAccount) << "looked up by our own Account ID";
  EXPECT_FALSE(ProfileIconLocalPath(dir_.string()).empty()) << "the `self` cache the Me page and Contacts read";
  EXPECT_EQ(fetched_urls, std::vector<std::string>{"https://cdn.example/me-1.png"});
}

TEST_F(OwnProfileIconSyncTest, ACachedPhotoThatIsStillCurrentIsNotFetchedAgain) {
  directory_.answer.icon = mesh::ProfileIconRef{"https://cdn.example/me-1.png", "b1", "png"};
  ASSERT_EQ(sync_->Refresh(), OwnProfileIconSync::Outcome::Fetched);
  Advance(OwnProfileIconSync::kLookupGapMs + 1);
  EXPECT_EQ(sync_->Refresh(), OwnProfileIconSync::Outcome::Unchanged);
  EXPECT_EQ(fetches, 1);
  EXPECT_EQ(directory_.lookups, 2);
}

TEST_F(OwnProfileIconSyncTest, APhotoChangedOnAnotherDeviceIsFetchedAgain) {
  directory_.answer.icon = mesh::ProfileIconRef{"https://cdn.example/me-1.png", "b1", "png"};
  ASSERT_EQ(sync_->Refresh(), OwnProfileIconSync::Outcome::Fetched);
  directory_.answer.icon = mesh::ProfileIconRef{"https://cdn.example/me-2.png", "b2", "png"};
  Advance(OwnProfileIconSync::kLookupGapMs + 1);
  EXPECT_EQ(sync_->Refresh(), OwnProfileIconSync::Outcome::Fetched);
  EXPECT_EQ(fetched_urls.back(), "https://cdn.example/me-2.png");
  auto meta = LoadProfileIconCacheMeta(dir_.string());
  ASSERT_TRUE(static_cast<bool>(meta));
  EXPECT_EQ(meta.value().blob_id, "b2");
}

TEST_F(OwnProfileIconSyncTest, AnAnswerIsNotAskedForAgainWithinTheGapButAFailedOneIs) {
  directory_.answer.icon = mesh::ProfileIconRef{"https://cdn.example/me-1.png", "b1", "png"};
  ASSERT_EQ(sync_->Refresh(), OwnProfileIconSync::Outcome::Fetched);
  EXPECT_EQ(sync_->Refresh(), OwnProfileIconSync::Outcome::Skipped);
  EXPECT_EQ(directory_.lookups, 1);

  OwnProfileIconSyncTest::TearDown();
  OwnProfileIconSyncTest::SetUp();
  directory_.fail = true;
  EXPECT_EQ(sync_->Refresh(), OwnProfileIconSync::Outcome::Failed);
  directory_.fail = false;
  directory_.answer.icon = mesh::ProfileIconRef{"https://cdn.example/me-1.png", "b1", "png"};
  EXPECT_EQ(sync_->Refresh(), OwnProfileIconSync::Outcome::Fetched) << "no wait after a failure";
}

TEST_F(OwnProfileIconSyncTest, AFailedDownloadLeavesNoPhotoAndTriesAgainNextTime) {
  directory_.answer.icon = mesh::ProfileIconRef{"https://cdn.example/me-1.png", "b1", "png"};
  fetch_fails = true;
  EXPECT_EQ(sync_->Refresh(), OwnProfileIconSync::Outcome::Failed);
  EXPECT_TRUE(ProfileIconLocalPath(dir_.string()).empty());
  fetch_fails = false;
  EXPECT_EQ(sync_->Refresh(), OwnProfileIconSync::Outcome::Fetched);
}

TEST_F(OwnProfileIconSyncTest, NoAccountYetMeansNoLookup) {
  account_.clear();
  EXPECT_EQ(sync_->Refresh(), OwnProfileIconSync::Outcome::Skipped);
  EXPECT_EQ(directory_.lookups, 0);
}

TEST_F(OwnProfileIconSyncTest, AnAnswerForAnotherAccountTakesNothing) {
  directory_.answer.account_id = "account:someone-else";
  directory_.answer.icon = mesh::ProfileIconRef{"https://cdn.example/other.png", "bx", "png"};
  EXPECT_EQ(sync_->Refresh(), OwnProfileIconSync::Outcome::Failed);
  EXPECT_EQ(fetches, 0);
  EXPECT_TRUE(ProfileIconLocalPath(dir_.string()).empty());
}

TEST_F(OwnProfileIconSyncTest, APhotoRemovedOnAnotherDeviceClearsTheCacheButNotAFreshOne) {
  directory_.answer.icon = mesh::ProfileIconRef{"https://cdn.example/me-1.png", "b1", "png"};
  ASSERT_EQ(sync_->Refresh(), OwnProfileIconSync::Outcome::Fetched);
  directory_.answer.icon.reset();

  Advance(OwnProfileIconSync::kLookupGapMs + 1);
  EXPECT_EQ(sync_->Refresh(), OwnProfileIconSync::Outcome::Unchanged) << "set a moment ago: the directory may lag";
  EXPECT_FALSE(ProfileIconLocalPath(dir_.string()).empty());

  Advance(OwnProfileIconSync::kClearGraceMs);
  EXPECT_EQ(sync_->Refresh(), OwnProfileIconSync::Outcome::Cleared);
  EXPECT_TRUE(ProfileIconLocalPath(dir_.string()).empty());
}

TEST_F(OwnProfileIconSyncTest, NoPhotoAnywhereChangesNothing) {
  EXPECT_EQ(sync_->Refresh(), OwnProfileIconSync::Outcome::Unchanged);
  EXPECT_EQ(fetches, 0);
}

} // namespace
} // namespace pbr
