#include "foundation/data/ProfileRegistry.h"
#include "foundation/data/ProfileManifest.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#include <string>

namespace {

std::string UniquePath(const char* prefix) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return (std::filesystem::temp_directory_path() /
          (std::string(prefix) + std::to_string(now)))
      .string();
}

} // namespace

TEST(ProfileRegistryTest, LoadsDefaultProfileAndManifest) {
  const std::string data_dir =
      UniquePath("pp_browser_profile_registry_test_");
  std::filesystem::remove_all(data_dir);

  auto registry = pbr::ProfileRegistry::Load(data_dir);
  ASSERT_TRUE(static_cast<bool>(registry));
  EXPECT_EQ(registry->ActiveProfileId(), "default");
  EXPECT_TRUE(std::filesystem::exists(registry->ActiveProfileDataDir()));
  {
    const auto manifest = pbr::EnsureProfileManifest(registry->ActiveProfileDataDir());
    ASSERT_TRUE(static_cast<bool>(manifest));
    EXPECT_TRUE(std::filesystem::exists(
        std::filesystem::path(registry->ActiveProfileDataDir()) / "manifest.json"));
  }

  std::filesystem::remove_all(data_dir);
}

TEST(ProfileRegistryTest, AppliesSessionProfileOverride) {
  const std::string override_dir =
      UniquePath("pp_browser_profile_override_test_");
  std::filesystem::remove_all(override_dir);

  auto registry = pbr::ProfileRegistry::Load(override_dir);
  ASSERT_TRUE(static_cast<bool>(registry));
  registry->SetSessionProfileOverride("dev");
  EXPECT_TRUE(registry->EnsureActiveProfile());

  const std::string expected = (std::filesystem::path(override_dir) / "profiles" / "dev").string();
  EXPECT_EQ(registry->ActiveProfileDataDir(), expected);
  EXPECT_TRUE(std::filesystem::exists(expected));

  std::filesystem::remove_all(override_dir);
}

// PB-041: a profile id names a directory under profiles/ and nothing else.
TEST(ProfileRegistryTest, AProfileIdCannotLeaveTheProfilesDirectory) {
  EXPECT_TRUE(pbr::ProfileRegistry::IsValidProfileId("default"));
  EXPECT_TRUE(pbr::ProfileRegistry::IsValidProfileId("node-2.test_B"));
  for (const char* bad : {"", "..", ".hidden", "../x", "a/b", "a\\b", "/etc", "C:x", "a b", "a\nb"}) {
    EXPECT_FALSE(pbr::ProfileRegistry::IsValidProfileId(bad)) << bad;
  }
  EXPECT_FALSE(pbr::ProfileRegistry::IsValidProfileId(std::string(65, 'a')));

  const std::string data_dir = UniquePath("pp_browser_profile_registry_escape_");
  std::filesystem::remove_all(data_dir);
  {
    auto registry = pbr::ProfileRegistry::Load(data_dir);
    ASSERT_TRUE(static_cast<bool>(registry));
    registry->SetSessionProfileOverride("../escaped");
    EXPECT_FALSE(static_cast<bool>(registry->EnsureActiveProfile()));
  }
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(data_dir) / "escaped"));
  std::filesystem::remove_all(data_dir);
}

TEST(ProfileRegistryTest, AStoredActiveIdThatIsNotAnIdFallsBackToTheDefault) {
  const std::string data_dir = UniquePath("pp_browser_profile_registry_stored_");
  std::filesystem::remove_all(data_dir);
  std::filesystem::create_directories(data_dir);
  {
    std::ofstream out(std::filesystem::path(data_dir) / "profiles.json");
    out << R"({"schema_version":1,"active_profile_id":"../../x","profiles":[{"id":"../../x","label":"x"},{"id":"work","label":"w"}]})";
  }
  auto registry = pbr::ProfileRegistry::Load(data_dir);
  ASSERT_TRUE(static_cast<bool>(registry));
  EXPECT_EQ(registry->ActiveProfileId(), "default");
  for (const pbr::ProfileEntry& entry : registry->Profiles()) {
    EXPECT_TRUE(pbr::ProfileRegistry::IsValidProfileId(entry.id)) << entry.id;
  }
  std::filesystem::remove_all(data_dir);
}
