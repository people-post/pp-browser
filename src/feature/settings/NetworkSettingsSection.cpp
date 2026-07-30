#include "feature/settings/NetworkSettingsSection.h"

#include "base/data/Config.h"
#include "base/data/Libp2pRole.h"
#include "base/data/SessionStore.h"
#include "base/i18n/LocalizationService.h"
#include "base/platform/Platform.h"
#include "feature/settings/SettingsLogic.h"

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
  state.node_enabled = bootstrap.config.libp2p.node_enabled ? "on" : "off";
  state.circuit_relay_enabled = bootstrap.config.libp2p.capabilities.circuit_relay ? "on" : "off";
  state.show_node_toggle = Platform::IsDesktop();
  Libp2pConfig libp2p = bootstrap.config.libp2p;
  NormalizeLibp2pConfig(libp2p);
  state.libp2p_listen_multiaddr = libp2p.listen_multiaddr;
  // libp2p_status_message is filled by SettingsController (needs MessagingHub).
}

bool NetworkSettingsSection::IsPersisted(const SettingsUiState& state, const BootstrapResult& bootstrap) const {
  const AppConfig& config = bootstrap.config;
  const bool node_on = state.node_enabled == "on";
  const bool circuit_on = state.circuit_relay_enabled == "on";
  return state.relay_base_url == config.relay.base_url && state.directory_base_url == config.directory.base_url &&
         state.registration_base_url == config.registration.base_url && node_on == config.libp2p.node_enabled &&
         circuit_on == config.libp2p.capabilities.circuit_relay;
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
  state.node_enabled = defaults.libp2p.node_enabled ? "on" : "off";
  state.circuit_relay_enabled = defaults.libp2p.capabilities.circuit_relay ? "on" : "off";
  state.show_node_toggle = Platform::IsDesktop();
  Libp2pConfig libp2p = defaults.libp2p;
  NormalizeLibp2pConfig(libp2p);
  state.libp2p_listen_multiaddr = libp2p.listen_multiaddr;
  state.libp2p_status_message.clear();
}

} // namespace pbr
