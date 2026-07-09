#include "feature/settings/LlmSettingsSection.h"

#include "base/data/LlmPreset.h"
#include "base/data/SessionStore.h"
#include "feature/settings/SettingsLogic.h"

namespace pbr {

namespace {

SettingsDraft ToSettingsDraft(const SettingsUiState& state) {
  SettingsDraft draft;
  draft.llm_preset = state.llm_preset;
  draft.llm_base_url = state.llm_base_url;
  draft.llm_model = state.llm_model;
  draft.llm_api_key = state.llm_api_key;
  draft.llm_api_key_env = state.llm_api_key_env;
  return draft;
}

} // namespace

const char* LlmSettingsSection::Id() const {
  return "llm";
}

SettingsSectionListItem LlmSettingsSection::ListItem() const {
  return {.id = Id(), .title = "Assistant", .subtitle = "Model, endpoint, API key"};
}

SettingsFlushMode LlmSettingsSection::FlushMode() const {
  return SettingsFlushMode::Debounced;
}

void LlmSettingsSection::SyncFromSession(const BootstrapResult& bootstrap, SettingsUiState& state) {
  const AppConfig& config = bootstrap.config;
  state.llm_preset = ResolvePreset(config);
  state.llm_base_url = config.llm.base_url;
  state.llm_model = config.llm.model;
  state.llm_api_key = config.llm.api_key;
  state.llm_api_key_env = bootstrap.config.llm_api_key_env;
}

bool LlmSettingsSection::IsPersisted(const SettingsUiState& state, const BootstrapResult& bootstrap) const {
  const AppConfig& config = bootstrap.config;
  return state.llm_preset == ResolvePreset(config) && state.llm_base_url == config.llm.base_url &&
         state.llm_model == config.llm.model && state.llm_api_key == config.llm.api_key &&
         state.llm_api_key_env == bootstrap.config.llm_api_key_env;
}

Roe<void> LlmSettingsSection::Flush(SettingsUiState& state, SessionStore& store) {
  const AppConfig config = ApplyLlmSettingsDraft(store.Snapshot().config, ToSettingsDraft(state));
  if (auto saved = store.SaveConfig(config); !saved) {
    return saved.error();
  }

  SyncFromSession(store.Snapshot(), state);
  return {};
}

void LlmSettingsSection::ResetToDefaults(SettingsUiState& state, const SessionStore& store) {
  const AppConfig defaults = store.DefaultConfig();
  state.llm_preset = ResolvePreset(defaults);
  state.llm_base_url = defaults.llm.base_url;
  state.llm_model = defaults.llm.model;
  state.llm_api_key = defaults.llm.api_key;
  state.llm_api_key_env = defaults.llm_api_key_env;
}

} // namespace pbr
