#include "lib/people/ProfileIconCache.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace {

using namespace pbr;

class ProfileIconCacheTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_profile_icon_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::string Save(const std::vector<uint8_t>& bytes) {
    EXPECT_TRUE(SaveProfileIconCache(dir_.string(), bytes, "jpg", ProfileIconCacheMeta{}));
    return ProfileIconLocalPath(dir_.string());
  }

  std::filesystem::path dir_;
};

// The UI caches a picture by its file path; a new photo at the old path kept
// showing the old one (Kenneth 2026-11-03: "无法更换头像").
TEST_F(ProfileIconCacheTest, ANewPictureGetsANewPathAndTheOldFileGoes) {
  const std::string first = Save({1, 2, 3});
  const std::string second = Save({4, 5, 6});

  ASSERT_FALSE(first.empty());
  ASSERT_FALSE(second.empty());
  EXPECT_NE(first, second);
  EXPECT_FALSE(std::filesystem::exists(first));
  EXPECT_TRUE(std::filesystem::exists(second));
}

TEST_F(ProfileIconCacheTest, TheSamePictureSavedAgainKeepsItsFile) {
  const std::string first = Save({7, 8, 9});
  const std::string again = Save({7, 8, 9});

  EXPECT_EQ(first, again);
  EXPECT_TRUE(std::filesystem::exists(again));
}

TEST_F(ProfileIconCacheTest, PictureAfterRemovingTheOldOneHasADifferentPath) {
  const std::string first = Save({1, 1, 1});
  ASSERT_TRUE(ClearProfileIconCache(dir_.string()));
  EXPECT_TRUE(ProfileIconLocalPath(dir_.string()).empty());

  const std::string next = Save({2, 2, 2});
  EXPECT_NE(first, next);
  EXPECT_TRUE(std::filesystem::exists(next));
}

} // namespace
