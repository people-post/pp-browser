// What the UI demo app does to its config and profile prefs, including after a reload from disk
// (DEV/PLAN-ui-demo-data.md); always compiled, so the normal build runs it.
#include "foundation/data/UiDemoConfig.h"
#include "foundation/data/Config.h"
#include "foundation/data/SessionStore.h"
#include "foundation/data/UserPreferences.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace pbr {
namespace {

std::string UniqueDir(const char* prefix) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto dir = std::filesystem::temp_directory_path() / (std::string(prefix) + std::to_string(now));
  std::filesystem::create_directories(dir);
  return dir.string();
}

void ExpectNoBackend(const AppConfig& config) {
  EXPECT_FALSE(config.mesh.mesh_enabled);
  EXPECT_TRUE(config.relay.base_url.empty());
  EXPECT_TRUE(config.directory.base_url.empty());
  EXPECT_TRUE(config.registration.base_url.empty());
  EXPECT_TRUE(config.llm.base_url.empty());
  EXPECT_TRUE(config.promoted_mcp.url.empty());
}

TEST(UiDemoConfigTest, EveryBackendAddressIsEmptyAndTheMeshIsOff) {
  AppConfig config = Config::DefaultAppConfig();
  config.backend.env = "custom";
  config.backend.has_saved_urls = true;
  config.backend.saved_relay.base_url = "https://saved.example.test/api/relay";
  config.relay.base_url = "https://backend.example.test/api/relay";
  config.directory.base_url = "https://backend.example.test/api/relay";
  config.directory.providers.push_back({"https://backend.example.test/api/relay", "http"});
  config.registration.base_url = "https://backend.example.test/api/relay";
  config.llm.base_url = "https://backend.example.test/api/llm/v1";
  config.promoted_mcp.url = "https://backend.example.test/mcp";
  McpConfig extra;
  extra.id = "remote";
  extra.url = "https://mcp.example.test";
  config.mcp_servers.push_back(extra);
  config.mesh.mesh_enabled = true;

  ApplyUiDemoConfig(config);

  EXPECT_EQ(config.backend.env, "prod");
  EXPECT_FALSE(config.backend.has_saved_urls);
  EXPECT_TRUE(config.backend.saved_relay.base_url.empty());
  EXPECT_TRUE(config.relay.base_url.empty());
  EXPECT_TRUE(config.directory.base_url.empty());
  EXPECT_TRUE(config.directory.providers.empty());
  EXPECT_TRUE(config.registration.base_url.empty());
  EXPECT_TRUE(config.llm.base_url.empty());
  EXPECT_TRUE(config.promoted_mcp.url.empty());
  ASSERT_EQ(config.mcp_servers.size(), 1u);
  EXPECT_TRUE(config.mcp_servers[0].url.empty());
  EXPECT_FALSE(config.mesh.mesh_enabled);
}

TEST(UiDemoConfigTest, OnboardingIsMarkedDone) {
  ProfilePreferences prefs;
  prefs.onboarding_completed = false;
  ApplyUiDemoProfilePrefs(prefs);
  EXPECT_TRUE(prefs.onboarding_completed);
}

TEST(UiDemoConfigTest, DefaultsAndFileLoadsComeOutAsTheDemo) {
  ExpectNoBackend(Config::DefaultAppConfig(true));
  const std::string dir = UniqueDir("pp_ui_demo_load_");
  const std::string path = dir + "/config.json";
  AppConfig saved = Config::DefaultAppConfig(false);
  saved.mesh.mesh_enabled = true;
  saved.relay.base_url = "https://backend.example.test/api/relay";
  ASSERT_TRUE(Config::SaveToFile(path, saved));
  auto loaded = Config::LoadFromFile(path, true);
  ASSERT_TRUE(static_cast<bool>(loaded));
  ExpectNoBackend(*loaded);
}

TEST(UiDemoConfigTest, ConfigReloadKeepsTheMeshOffAndEveryAddressEmpty) {
  const std::string dir = UniqueDir("pp_ui_demo_reload_");
  BootstrapResult bootstrap{};
  bootstrap.config = Config::DefaultAppConfig(true);
  bootstrap.config_path = dir + "/config.json";  // no file: ReloadConfig falls back to defaults
  bootstrap.profile_data_dir = dir;
  bootstrap.profile_prefs = UserPreferences::DefaultProfile();
  SessionStore store;
  store.Initialize(std::move(bootstrap));
  store.SetUiDemo(true);

  ASSERT_TRUE(store.ReloadConfig());
  ExpectNoBackend(store.Snapshot().config);

  AppConfig edited = store.Snapshot().config;
  edited.llm.model = "edited";
  ASSERT_TRUE(store.SaveConfig(edited));  // writes the file, so the next reload takes the load-from-file path
  ASSERT_TRUE(store.ReloadConfig());
  ExpectNoBackend(store.Snapshot().config);
  EXPECT_EQ(store.Snapshot().config.llm.model, "edited");
}

TEST(UiDemoConfigTest, ProfilePrefsReloadKeepsOnboardingCompleted) {
  const std::string dir = UniqueDir("pp_ui_demo_prefs_");
  BootstrapResult bootstrap{};
  bootstrap.config = Config::DefaultAppConfig(true);
  bootstrap.config_path = dir + "/config.json";
  bootstrap.profile_data_dir = dir;
  bootstrap.profile_prefs = UserPreferences::DefaultProfile();
  ASSERT_FALSE(bootstrap.profile_prefs.onboarding_completed);
  SessionStore store;
  store.Initialize(std::move(bootstrap));
  store.SetUiDemo(true);

  ASSERT_TRUE(store.ReloadProfilePrefs());  // fresh profile dir: the file holds the default (not completed)
  EXPECT_TRUE(store.Snapshot().profile_prefs.onboarding_completed);

  ProfilePreferences reset = store.Snapshot().profile_prefs;
  reset.onboarding_completed = false;
  ASSERT_TRUE(store.SaveProfilePrefs(reset));
  EXPECT_TRUE(store.Snapshot().profile_prefs.onboarding_completed);
}

} // namespace
} // namespace pbr
