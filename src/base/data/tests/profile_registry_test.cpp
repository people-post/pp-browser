#include "base/data/ProfileRegistry.h"
#include "base/data/SchemaVersion.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

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
    const auto manifest = pbr::SchemaVersion::EnsureProfileManifest(registry->ActiveProfileDataDir());
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
