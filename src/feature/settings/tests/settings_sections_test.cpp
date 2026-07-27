#include "feature/settings/AboutSettingsSection.h"
#include "feature/settings/AppearanceSettingsSection.h"
#include "feature/settings/IntegrationsSettingsSection.h"
#include "feature/settings/LlmSettingsSection.h"
#include "feature/settings/NetworkSettingsSection.h"
#include "feature/settings/StorageSettingsSection.h"

#include "base/i18n/LocalizationService.h"

#include <gtest/gtest.h>

namespace {

void LoadLocales() {
#ifdef PP_BROWSER_ASSETS_DIR
  ASSERT_TRUE(pbr::LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
#endif
  pbr::LocalizationService::Instance().SetPreferredLanguage("en");
}

} // namespace

TEST(SettingsSectionsTest, SyncAndPersistenceSignals) {
  LoadLocales();
  pbr::BootstrapResult bootstrap;
  bootstrap.config.llm.preset = "cloud";
  bootstrap.config.llm.base_url = "https://api.openai.com/v1";
  bootstrap.config.llm.model = "gpt-4o-mini";
  bootstrap.config.llm.api_key = "saved-key";
  bootstrap.config.promoted_mcp.url = "https://promoted.example/mcp";
  bootstrap.config.search.provider = "duckduckgo";
  bootstrap.config.mcp_servers.push_back(
      {.id = "custom", .url = "https://custom.example/mcp", .enabled = true});
  bootstrap.config.relay.base_url = "https://relay.example";
  bootstrap.config.directory.base_url = "https://directory.example";
  bootstrap.config.registration.base_url = "https://registration.example";
  bootstrap.profile_prefs.appearance = "dark";
  bootstrap.data_dir = "/tmp/data";
  bootstrap.profile_data_dir = "/tmp/data/profiles/default";

  pbr::SettingsUiState state;
  pbr::LlmSettingsSection llm_section;
  llm_section.SyncFromSession(bootstrap, state);
  EXPECT_EQ(state.llm_model, "gpt-4o-mini");

  pbr::IntegrationsSettingsSection integrations_section;
  integrations_section.SyncFromSession(bootstrap, state);
  EXPECT_EQ(state.promoted_mcp_url, "https://promoted.example/mcp");
  EXPECT_EQ(state.mcp_servers.size(), 1U);
  EXPECT_TRUE(integrations_section.IsPersisted(state, bootstrap));

  pbr::NetworkSettingsSection network_section;
  network_section.SyncFromSession(bootstrap, state);
  EXPECT_EQ(state.relay_base_url, "https://relay.example");
  EXPECT_EQ(state.node_enabled, "on");
  EXPECT_FALSE(state.libp2p_listen_multiaddr.empty());
  EXPECT_TRUE(network_section.IsPersisted(state, bootstrap));

  state.node_enabled = "off";
  EXPECT_FALSE(network_section.IsPersisted(state, bootstrap));
  state.node_enabled = "on";
  EXPECT_TRUE(network_section.IsPersisted(state, bootstrap));

  pbr::AppearanceSettingsSection appearance_section;
  appearance_section.SyncFromSession(bootstrap, state);
  EXPECT_EQ(state.appearance, "dark");
  EXPECT_EQ(state.appearance_label, "Dark");
  EXPECT_EQ(state.reduce_transparency, "off");

  bootstrap.profile_prefs.reduce_transparency = true;
  EXPECT_FALSE(appearance_section.IsPersisted(state, bootstrap));
  appearance_section.SyncFromSession(bootstrap, state);
  EXPECT_EQ(state.reduce_transparency, "on");
  EXPECT_TRUE(appearance_section.IsPersisted(state, bootstrap));

  pbr::StorageSettingsSection storage_section;
  storage_section.SyncFromSession(bootstrap, state);
  EXPECT_EQ(state.data_dir, "/tmp/data");
  EXPECT_FALSE(state.profile_size_label.empty());
  EXPECT_NE(state.profile_size_label.find("Profile uses ~"), std::string::npos);
  EXPECT_FALSE(storage_section.IsWritable());

  pbr::AboutSettingsSection about_section;
  about_section.SyncFromSession(bootstrap, state);
  EXPECT_FALSE(state.app_name.empty());
  EXPECT_FALSE(state.app_version.empty());
  EXPECT_FALSE(about_section.IsWritable());
}
