#include "feature/settings/NetworkSettingsSection.h"

#include "base/data/Config.h"
#include "base/data/SessionStore.h"
#include "foundation/i18n/LocalizationService.h"
#include "base/platform/Platform.h"
#include "feature/settings/SettingsLogic.h"
#include "common/PbrCompat.h"

namespace pbr {

const char* NetworkSettingsSection::Id() const {
  return "network";
}

SettingsSectionListItem NetworkSettingsSection::ListItem() const {
  return {.id = Id(), .title = Tr("settings.network.title"), .subtitle = Tr("settings.network.subtitle")};
}

SettingsFlushMode NetworkSettingsSection::FlushMode() const {
  return SettingsFlushMode::Debounced;
}

void NetworkSettingsSection::SyncFromSession(const BootstrapResult& bootstrap, SettingsUiState& state) {
  state.relay_base_url = bootstrap.config.relay.base_url;
  state.directory_base_url = bootstrap.config.directory.base_url;
  state.registration_base_url = bootstrap.config.registration.base_url;
  state.node_enabled = bootstrap.config.mesh.node_enabled ? "on" : "off";
  state.circuit_relay_enabled = bootstrap.config.mesh.capabilities.circuit_relay ? "on" : "off";
  state.media_relay_enabled = bootstrap.config.mesh.capabilities.media_relay ? "on" : "off";
  state.dht_enabled = bootstrap.config.mesh.capabilities.dht ? "on" : "off";
  state.prefer_contacts_for_routing = bootstrap.config.mesh.prefer_contacts_for_routing ? "on" : "off";
  state.show_node_toggle = Platform::IsDesktop();
  // amp_listen_multiaddr is filled by SettingsController from MessagingHub runtime.
}

bool NetworkSettingsSection::IsPersisted(const SettingsUiState& state, const BootstrapResult& bootstrap) const {
  const AppConfig& config = bootstrap.config;
  const bool node_on = state.node_enabled == "on";
  const bool circuit_on = state.circuit_relay_enabled == "on";
  const bool media_on = state.media_relay_enabled == "on";
  const bool dht_on = state.dht_enabled == "on";
  const bool prefer_contacts = state.prefer_contacts_for_routing == "on";
  return state.relay_base_url == config.relay.base_url && state.directory_base_url == config.directory.base_url &&
         state.registration_base_url == config.registration.base_url && node_on == config.mesh.node_enabled &&
         circuit_on == config.mesh.capabilities.circuit_relay &&
         media_on == config.mesh.capabilities.media_relay && dht_on == config.mesh.capabilities.dht &&
         prefer_contacts == config.mesh.prefer_contacts_for_routing;
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
  state.node_enabled = defaults.mesh.node_enabled ? "on" : "off";
  state.circuit_relay_enabled = defaults.mesh.capabilities.circuit_relay ? "on" : "off";
  state.media_relay_enabled = defaults.mesh.capabilities.media_relay ? "on" : "off";
  state.dht_enabled = defaults.mesh.capabilities.dht ? "on" : "off";
  state.prefer_contacts_for_routing = defaults.mesh.prefer_contacts_for_routing ? "on" : "off";
  state.show_node_toggle = Platform::IsDesktop();
  state.mesh_status_message.clear();
}

} // namespace pbr
