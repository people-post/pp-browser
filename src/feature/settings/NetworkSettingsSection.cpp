#include "feature/settings/NetworkSettingsSection.h"

#include "base/data/Config.h"
#include "base/data/SessionStore.h"
#include "feature/settings/SettingsLogic.h"

namespace pbr {

const char* NetworkSettingsSection::Id() const {
  return "network";
}

SettingsSectionListItem NetworkSettingsSection::ListItem() const {
  return {.id = Id(), .title = "Network", .subtitle = "Relay, directory, registration"};
}

SettingsFlushMode NetworkSettingsSection::FlushMode() const {
  return SettingsFlushMode::Debounced;
}

void NetworkSettingsSection::SyncFromSession(const BootstrapResult& bootstrap, SettingsUiState& state) {
  state.relay_base_url = bootstrap.config.relay.base_url;
  state.directory_base_url = bootstrap.config.directory.base_url;
  state.registration_base_url = bootstrap.config.registration.base_url;
}

bool NetworkSettingsSection::IsPersisted(const SettingsUiState& state, const BootstrapResult& bootstrap) const {
  const AppConfig& config = bootstrap.config;
  return state.relay_base_url == config.relay.base_url && state.directory_base_url == config.directory.base_url &&
         state.registration_base_url == config.registration.base_url;
}

Roe<void> NetworkSettingsSection::Flush(SettingsUiState& state, SessionStore& store) {
  AppConfig config = ApplyNetworkSettingsDraft(store.Snapshot().config, state);
  if (auto saved = store.SaveConfig(config); !saved) {
    return saved.error();
  }
  SyncFromSession(store.Snapshot(), state);
  return {};
}

void NetworkSettingsSection::ResetToDefaults(SettingsUiState& state, const SessionStore& /*store*/) {
  const AppConfig defaults = Config::DefaultAppConfig();
  state.relay_base_url = defaults.relay.base_url;
  state.directory_base_url = defaults.directory.base_url;
  state.registration_base_url = defaults.registration.base_url;
}

} // namespace pbr
