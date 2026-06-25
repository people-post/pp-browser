#include "feature/settings/AppearanceSettingsSection.h"
#include "feature/settings/LlmSettingsSection.h"
#include "feature/settings/StorageSettingsSection.h"

#include <cassert>
#include <iostream>

int main() {
  pbr::BootstrapResult bootstrap;
  bootstrap.config.llm.preset = "cloud";
  bootstrap.config.llm.base_url = "https://api.openai.com/v1";
  bootstrap.config.llm.model = "gpt-4o-mini";
  bootstrap.config.llm.api_key = "saved-key";
  bootstrap.profile_prefs.appearance = "dark";
  bootstrap.data_dir = "/tmp/data";
  bootstrap.profile_data_dir = "/tmp/data/profiles/default";

  pbr::SettingsUiState state;
  pbr::LlmSettingsSection llm_section;
  llm_section.SyncFromSession(bootstrap, state);
  assert(state.llm_model == "gpt-4o-mini");
  assert(state.llm_api_key == "saved-key");
  assert(llm_section.IsPersisted(state, bootstrap));

  state.llm_model = "gpt-4o";
  assert(!llm_section.IsPersisted(state, bootstrap));

  pbr::AppearanceSettingsSection appearance_section;
  appearance_section.SyncFromSession(bootstrap, state);
  assert(state.appearance == "dark");
  assert(appearance_section.IsPersisted(state, bootstrap));
  state.appearance = "light";
  assert(!appearance_section.IsPersisted(state, bootstrap));

  pbr::StorageSettingsSection storage_section;
  storage_section.SyncFromSession(bootstrap, state);
  assert(state.data_dir == "/tmp/data");
  assert(state.profile_dir == "/tmp/data/profiles/default");
  assert(!storage_section.IsWritable());
  assert(storage_section.IsPersisted(state, bootstrap));

  std::cout << "settings_sections_test ok\n";
  return 0;
}
