#include "foundation/data/Config.h"
#include "foundation/data/SessionStore.h"
#include "foundation/data/UserPreferences.h"

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

TEST(SessionStoreTest, SavesAndReloadsConfigAndProfilePreferences) {
  const std::string config_path =
      UniquePath("pp_browser_session_store_test_") + ".json";
  const std::string profile_dir = UniquePath("pp_browser_session_store_profile_");
  const std::string data_dir = UniquePath("pp_browser_session_store_data_");

  std::filesystem::create_directories(profile_dir);
  std::filesystem::create_directories(data_dir);

  pbr::BootstrapResult bootstrap{};
  bootstrap.config = pbr::Config::DefaultAppConfig();
  bootstrap.config_path = config_path;
  bootstrap.data_dir = data_dir;
  bootstrap.profile_data_dir = profile_dir;
  bootstrap.profile_prefs = pbr::UserPreferences::DefaultProfile();
  bootstrap.profile_prefs.theme = "themes/base.rcss";

  pbr::SessionStore store;
  store.Initialize(std::move(bootstrap));

  pbr::AppConfig edited = store.Snapshot().config;
  edited.llm.model = "edited-model";
  edited.llm.preset = "cloud";

  EXPECT_TRUE(store.SaveConfig(edited));
  EXPECT_EQ(store.Snapshot().config.llm.model, "edited-model");

  auto from_disk = pbr::Config::LoadFromFile(config_path);
  ASSERT_TRUE(static_cast<bool>(from_disk));
  EXPECT_EQ(from_disk->llm.model, "edited-model");

  pbr::ProfilePreferences prefs = store.Snapshot().profile_prefs;
  prefs.appearance = "dark";
  EXPECT_TRUE(store.SaveProfilePrefs(prefs));
  EXPECT_EQ(store.Snapshot().profile_prefs.appearance, "dark");

  auto reloaded_prefs = pbr::UserPreferences::LoadProfile(profile_dir);
  ASSERT_TRUE(static_cast<bool>(reloaded_prefs));
  EXPECT_EQ(reloaded_prefs->appearance, "dark");

  EXPECT_TRUE(store.ReloadProfilePrefs());
  EXPECT_EQ(store.Snapshot().profile_prefs.appearance, "dark");

  prefs.appearance = "light";
  EXPECT_TRUE(pbr::UserPreferences::SaveProfile(profile_dir, prefs));
  EXPECT_TRUE(store.ReloadProfilePrefs());
  EXPECT_EQ(store.Snapshot().profile_prefs.appearance, "light");

  std::filesystem::remove(config_path);
  std::filesystem::remove_all(profile_dir);
  std::filesystem::remove_all(data_dir);
}

TEST(SessionStoreTest, ReloadFromDiskUsesDefaultsWhenConfigMissing) {
  const std::string config_path =
      UniquePath("pp_browser_session_store_missing_config_") + ".json";
  const std::string profile_dir = UniquePath("pp_browser_session_store_missing_profile_");
  const std::string data_dir = UniquePath("pp_browser_session_store_missing_data_");

  std::filesystem::create_directories(profile_dir);
  std::filesystem::create_directories(data_dir);
  ASSERT_FALSE(std::filesystem::exists(config_path));

  pbr::BootstrapResult bootstrap{};
  bootstrap.config = pbr::Config::DefaultAppConfig();
  bootstrap.config.llm.model = "in-memory-only";
  bootstrap.config_path = config_path;
  bootstrap.data_dir = data_dir;
  bootstrap.profile_data_dir = profile_dir;
  bootstrap.profile_prefs = pbr::UserPreferences::DefaultProfile();

  pbr::SessionStore store;
  store.Initialize(std::move(bootstrap));

  EXPECT_TRUE(store.ReloadFromDisk());
  EXPECT_EQ(store.Snapshot().config.llm.model,
            pbr::Config::DefaultAppConfig().llm.model);

  std::filesystem::remove_all(profile_dir);
  std::filesystem::remove_all(data_dir);
}

TEST(SessionStoreTest, ReloadConfigSkipsListenerWhenUnchanged) {
  const std::string config_path =
      UniquePath("pp_browser_session_store_reload_unchanged_") + ".json";
  const std::string profile_dir = UniquePath("pp_browser_session_store_reload_unchanged_profile_");
  const std::string data_dir = UniquePath("pp_browser_session_store_reload_unchanged_data_");

  std::filesystem::create_directories(profile_dir);
  std::filesystem::create_directories(data_dir);

  pbr::BootstrapResult bootstrap{};
  bootstrap.config = pbr::Config::DefaultAppConfig();
  bootstrap.config.llm.model = "stable-model";
  bootstrap.config_path = config_path;
  bootstrap.data_dir = data_dir;
  bootstrap.profile_data_dir = profile_dir;
  bootstrap.profile_prefs = pbr::UserPreferences::DefaultProfile();

  pbr::SessionStore store;
  store.Initialize(std::move(bootstrap));
  ASSERT_TRUE(store.SaveConfig(store.Snapshot().config));

  int notify_count = 0;
  store.AddConfigListener([&](const pbr::AppConfig&) { ++notify_count; });

  EXPECT_TRUE(store.ReloadConfig());
  EXPECT_EQ(notify_count, 0);

  pbr::AppConfig edited = store.Snapshot().config;
  edited.llm.model = "changed-model";
  ASSERT_TRUE(pbr::Config::SaveToFile(config_path, edited));

  EXPECT_TRUE(store.ReloadConfig());
  EXPECT_EQ(notify_count, 1);
  EXPECT_EQ(store.Snapshot().config.llm.model, "changed-model");

  std::filesystem::remove(config_path);
  std::filesystem::remove_all(profile_dir);
  std::filesystem::remove_all(data_dir);
}

TEST(SessionStoreTest, ProfilePrefsListenerFiresOnRelevantChange) {
  const std::string config_path =
      UniquePath("pp_browser_session_store_prefs_listener_") + ".json";
  const std::string profile_dir = UniquePath("pp_browser_session_store_prefs_listener_profile_");
  const std::string data_dir = UniquePath("pp_browser_session_store_prefs_listener_data_");

  std::filesystem::create_directories(profile_dir);
  std::filesystem::create_directories(data_dir);

  pbr::BootstrapResult bootstrap{};
  bootstrap.config = pbr::Config::DefaultAppConfig();
  bootstrap.config_path = config_path;
  bootstrap.data_dir = data_dir;
  bootstrap.profile_data_dir = profile_dir;
  bootstrap.profile_prefs = pbr::UserPreferences::DefaultProfile();
  bootstrap.profile_prefs.group_invite_policy = "contacts_only";

  pbr::SessionStore store;
  store.Initialize(std::move(bootstrap));

  int prefs_notify = 0;
  int appearance_notify = 0;
  std::string last_policy;
  store.AddProfilePrefsListener([&](const pbr::ProfilePreferences& prefs) {
    ++prefs_notify;
    last_policy = prefs.group_invite_policy;
  });
  store.AddAppearanceListener(
      [&](const std::string&) { ++appearance_notify; });

  pbr::ProfilePreferences prefs = store.Snapshot().profile_prefs;
  EXPECT_TRUE(store.SaveProfilePrefs(prefs));
  EXPECT_EQ(prefs_notify, 0);

  prefs.group_invite_policy = "everyone";
  EXPECT_TRUE(store.SaveProfilePrefs(prefs));
  EXPECT_EQ(prefs_notify, 1);
  EXPECT_EQ(last_policy, "everyone");
  EXPECT_EQ(appearance_notify, 0);

  prefs.appearance = "dark";
  EXPECT_TRUE(store.SaveProfilePrefs(prefs));
  EXPECT_EQ(prefs_notify, 2);
  EXPECT_EQ(appearance_notify, 1);

  std::filesystem::remove(config_path);
  std::filesystem::remove_all(profile_dir);
  std::filesystem::remove_all(data_dir);
}
