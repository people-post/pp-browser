#include "feature/settings/AboutSettingsSection.h"
#include "feature/settings/AppearanceSettingsSection.h"
#include "feature/settings/IntegrationsSettingsSection.h"
#include "feature/settings/LlmSettingsSection.h"
#include "feature/settings/NetworkSettingsSection.h"
#include "feature/settings/StorageSettingsSection.h"

#include "foundation/i18n/LocalizationService.h"

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
  EXPECT_TRUE(state.amp_listen_multiaddr.empty());
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
  EXPECT_TRUE(storage_section.IsWritable());

  pbr::AboutSettingsSection about_section;
  about_section.SyncFromSession(bootstrap, state);
  EXPECT_FALSE(state.app_name.empty());
  EXPECT_FALSE(state.app_version.empty());
  EXPECT_TRUE(about_section.IsWritable()) << "About owns the Show advanced settings switch";
}

// Me → About → "Show advanced settings": off for everyone by default. The Integrations (MCP) section is listed
// only while it is on; the switch itself lives in the About section and persists to the profile prefs.
TEST(SettingsSectionsTest, AdvancedSettingsSwitchAndAdvancedOnlySections) {
  LoadLocales();
  EXPECT_TRUE(pbr::IntegrationsSettingsSection().ListItem().advanced_only);
  EXPECT_FALSE(pbr::AboutSettingsSection().ListItem().advanced_only);
  EXPECT_FALSE(pbr::NetworkSettingsSection().ListItem().advanced_only);

  pbr::BootstrapResult bootstrap;
  pbr::SettingsUiState state;
  pbr::AboutSettingsSection about;
  about.SyncFromSession(bootstrap, state);
  EXPECT_EQ(state.show_advanced_settings, "off");
  EXPECT_TRUE(about.IsPersisted(state, bootstrap));
  state.show_advanced_settings = "on";
  EXPECT_FALSE(about.IsPersisted(state, bootstrap));
  bootstrap.profile_prefs.show_advanced_settings = true;
  EXPECT_TRUE(about.IsPersisted(state, bootstrap));
}

// Me → Storage → Auto-download: three choices; "everything" has a Wi-Fi and a mobile-network tick on phones only,
// and unticking the last one falls back to the default choice.
TEST(SettingsSectionsTest, AutoDownloadChoicesTicksAndFallback) {
  LoadLocales();
  pbr::BootstrapResult bootstrap;
  pbr::SettingsUiState state;
  pbr::StorageSettingsSection section;
  section.SyncFromSession(bootstrap, state);
  EXPECT_EQ(state.attachment_download_policy, "smart");
  EXPECT_EQ(state.attachment_download_policy_label, "Up to 4 MB (default)");
  EXPECT_FALSE(pbr::AttachmentAutoTogglesVisible(state, pbr::PlatformKind::IOS));

  pbr::ChooseAttachmentDownloadPolicy(state, "on_demand");
  EXPECT_EQ(state.attachment_download_policy_label, "Nothing (saves data)");
  EXPECT_FALSE(pbr::AttachmentAutoTogglesVisible(state, pbr::PlatformKind::Android));

  pbr::ChooseAttachmentDownloadPolicy(state, "always_auto");
  EXPECT_EQ(state.attachment_auto_on_wifi, "on");
  EXPECT_EQ(state.attachment_auto_on_cellular, "off");
  EXPECT_TRUE(pbr::AttachmentAutoTogglesVisible(state, pbr::PlatformKind::IOS));
  EXPECT_TRUE(pbr::AttachmentAutoTogglesVisible(state, pbr::PlatformKind::Android));
  EXPECT_FALSE(pbr::AttachmentAutoTogglesVisible(state, pbr::PlatformKind::Desktop));

  EXPECT_FALSE(section.IsPersisted(state, bootstrap));
  pbr::ToggleAttachmentAutoNetwork(state, /*cellular=*/true);
  EXPECT_EQ(state.attachment_download_policy, "always_auto");
  EXPECT_EQ(state.attachment_auto_on_cellular, "on");
  pbr::ToggleAttachmentAutoNetwork(state, /*cellular=*/false);
  EXPECT_EQ(state.attachment_download_policy, "always_auto") << "one tick is still on";
  EXPECT_EQ(state.attachment_auto_on_wifi, "off");
  pbr::ToggleAttachmentAutoNetwork(state, /*cellular=*/true);
  EXPECT_EQ(state.attachment_download_policy, "smart") << "the last tick off returns to the default";
  EXPECT_EQ(state.attachment_download_policy_label, "Up to 4 MB (default)");
  EXPECT_FALSE(pbr::AttachmentAutoTogglesVisible(state, pbr::PlatformKind::IOS));
  EXPECT_TRUE(section.IsPersisted(state, bootstrap));

  bootstrap.profile_prefs.attachment_download_policy = "always_auto";
  bootstrap.profile_prefs.attachment_auto_on_wifi = false;
  bootstrap.profile_prefs.attachment_auto_on_cellular = true;
  section.SyncFromSession(bootstrap, state);
  EXPECT_EQ(state.attachment_auto_on_wifi, "off");
  EXPECT_EQ(state.attachment_auto_on_cellular, "on");
  EXPECT_TRUE(section.IsPersisted(state, bootstrap));
}
