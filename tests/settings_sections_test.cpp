#include "feature/settings/AppearanceSettingsSection.h"
#include "feature/settings/IntegrationsSettingsSection.h"
#include "feature/settings/LlmSettingsSection.h"
#include "feature/settings/NetworkSettingsSection.h"
#include "feature/settings/StorageSettingsSection.h"

#include <cassert>
#include <iostream>

int main() {
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
  assert(state.llm_model == "gpt-4o-mini");

  pbr::IntegrationsSettingsSection integrations_section;
  integrations_section.SyncFromSession(bootstrap, state);
  assert(state.promoted_mcp_url == "https://promoted.example/mcp");
  assert(state.mcp_servers.size() == 1);
  assert(integrations_section.IsPersisted(state, bootstrap));

  pbr::NetworkSettingsSection network_section;
  network_section.SyncFromSession(bootstrap, state);
  assert(state.relay_base_url == "https://relay.example");
  assert(network_section.IsPersisted(state, bootstrap));

  pbr::AppearanceSettingsSection appearance_section;
  appearance_section.SyncFromSession(bootstrap, state);
  assert(state.appearance == "dark");

  pbr::StorageSettingsSection storage_section;
  storage_section.SyncFromSession(bootstrap, state);
  assert(state.data_dir == "/tmp/data");
  assert(!storage_section.IsWritable());

  std::cout << "settings_sections_test ok\n";
  return 0;
}
