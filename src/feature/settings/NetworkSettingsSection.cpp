#include "feature/settings/NetworkSettingsSection.h"
#include "foundation/data/BackendProfile.h"

#include "foundation/data/Config.h"
#include "foundation/data/SessionStore.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/BackendTable.h"
#include "foundation/platform/DeploymentProfile.h"
#include "foundation/platform/Platform.h"
#include "foundation/product/DevTools.h"
#include "feature/settings/SettingsLogic.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

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
  state.backend_env = bootstrap.config.backend.env;
  state.backend_services = BackendServiceEntries(ActiveBackendTable(), bootstrap.config.promoted_mcp.IsConfigured());
  state.backend_prod_available = BackendAvailable(BackendEnv::Prod);
  state.backend_staging_available = BackendAvailable(BackendEnv::Staging);
  state.backend_dev_available = BackendAvailable(BackendEnv::Dev);
  state.backend_restart_pending = BackendSwitchPending(bootstrap.config, bootstrap.backend_key);
  // Prod / staging / dev derive the URLs from the origin; the fields edit the custom ones.
  const BackendConfig& backend = bootstrap.config.backend;
  const bool saved = backend.has_saved_urls;
  // The relay and registration fields show relay links (MR008): the address with the PeerId it must answer to.
  state.relay_base_url = EndpointLinkText(saved ? backend.saved_relay : bootstrap.config.relay);
  state.directory_base_url = saved ? backend.saved_directory.base_url : bootstrap.config.directory.base_url;
  state.registration_base_url = EndpointLinkText(saved ? backend.saved_registration : bootstrap.config.registration);
  state.service_account_locker = backend.services.account_locker_url;
  state.service_client_compat = backend.services.client_compat_url;
  state.service_llm_gateway = backend.services.llm_gateway_url;
  state.service_llm_token = backend.services.llm_token_url;
  state.service_crash_intake = backend.services.crash_intake_url;
  state.service_mesh_nodes = backend.services.mesh_nodes_url;
  state.service_promoted_mcp = backend.services.promoted_mcp_url;
  state.backend_services_missing.clear();
  for (const BackendServiceEntry& entry : state.backend_services) {
    if (!entry.available && !entry.reason.empty()) {
      state.backend_services_missing += (state.backend_services_missing.empty() ? "" : " ") + entry.reason;
    }
  }
  state.relay_id_missing = !MissingRelayIdSetting(bootstrap.config, kDeveloperRelaysAllowed,
                                                  !OriginEnvOverrideActive() && ActiveBackend() != BackendEnv::Custom)
                                .empty();
  state.registration_required = false;
  state.node_enabled = bootstrap.config.mesh.node_enabled ? "on" : "off";
  state.circuit_relay_enabled = bootstrap.config.mesh.capabilities.circuit_relay ? "on" : "off";
  state.media_relay_enabled = bootstrap.config.mesh.capabilities.media_relay ? "on" : "off";
  state.dht_enabled = bootstrap.config.mesh.capabilities.dht ? "on" : "off";
  state.prefer_contacts_for_routing = bootstrap.config.mesh.prefer_contacts_for_routing ? "on" : "off";
  state.direct_connections = mesh::DirectAudienceName(bootstrap.config.mesh.direct_connections);
  state.trusted_relays_only = bootstrap.config.mesh.trusted_relays_only ? "on" : "off";
  state.reachable_without_relay = bootstrap.config.mesh.reachable_without_relay ? "on" : "off";
  state.show_node_toggle = Platform::IsDesktop();
  // amp_listen_multiaddr is filled by SettingsController from ConversationsHub runtime.
}

bool NetworkSettingsSection::IsPersisted(const SettingsUiState& state, const BootstrapResult& bootstrap) const {
  const AppConfig& config = bootstrap.config;
  const bool node_on = state.node_enabled == "on";
  const bool circuit_on = state.circuit_relay_enabled == "on";
  const bool media_on = state.media_relay_enabled == "on";
  const bool dht_on = state.dht_enabled == "on";
  const bool prefer_contacts = state.prefer_contacts_for_routing == "on";
  const bool saved = config.backend.has_saved_urls;
  const std::string relay = EndpointLinkText(saved ? config.backend.saved_relay : config.relay);
  const std::string& directory = saved ? config.backend.saved_directory.base_url : config.directory.base_url;
  const std::string registration = EndpointLinkText(saved ? config.backend.saved_registration : config.registration);
  return (state.backend_env.empty() || state.backend_env == config.backend.env) && state.relay_base_url == relay &&
         state.directory_base_url == directory && state.registration_base_url == registration &&
         state.service_account_locker == config.backend.services.account_locker_url &&
         state.service_client_compat == config.backend.services.client_compat_url &&
         state.service_llm_gateway == config.backend.services.llm_gateway_url &&
         state.service_llm_token == config.backend.services.llm_token_url &&
         state.service_crash_intake == config.backend.services.crash_intake_url &&
         state.service_mesh_nodes == config.backend.services.mesh_nodes_url &&
         state.service_promoted_mcp == config.backend.services.promoted_mcp_url &&
         node_on == config.mesh.node_enabled && circuit_on == config.mesh.capabilities.circuit_relay &&
         media_on == config.mesh.capabilities.media_relay && dht_on == config.mesh.capabilities.dht &&
         prefer_contacts == config.mesh.prefer_contacts_for_routing &&
         state.direct_connections == mesh::DirectAudienceName(config.mesh.direct_connections) &&
         (state.trusted_relays_only == "on") == config.mesh.trusted_relays_only &&
         (state.reachable_without_relay == "on") == config.mesh.reachable_without_relay;
}

Roe<void> NetworkSettingsSection::Flush(SettingsUiState& state, SessionStore& store) {
  // A custom backend's account server URL names its account (profile folder, registration): an empty one would not
  // mean "none" but a different backend's profile, so it is refused, not saved (the page shows why).
  if ((state.backend_env.empty() || state.backend_env == "custom") && !store.Snapshot().config.backend.env.empty() &&
      ParseBackendEnv(store.Snapshot().config.backend.env) == BackendEnv::Custom &&
      state.registration_base_url.find_first_not_of(" \t") == std::string::npos) {
    state.registration_required = true;
    return Error(Tr("settings.network.registration_required"));
  }
  AppConfig config = ApplyNetworkSettingsDraft(store.Snapshot().config, state);
  if (auto saved = store.SaveConfig(config); !saved) {
    return saved.error();
  }
  SyncFromSession(store.Snapshot(), state);
  return {};
}

void NetworkSettingsSection::ResetToDefaults(SettingsUiState& state, const SessionStore& /*store*/) {
  const AppConfig defaults = Config::DefaultAppConfig();
  state.backend_env = defaults.backend.env;
  state.relay_base_url = EndpointLinkText(defaults.relay);
  state.directory_base_url = defaults.directory.base_url;
  state.registration_base_url = EndpointLinkText(defaults.registration);
  state.service_account_locker.clear();
  state.service_client_compat.clear();
  state.service_llm_gateway.clear();
  state.service_llm_token.clear();
  state.service_crash_intake.clear();
  state.service_mesh_nodes.clear();
  state.service_promoted_mcp.clear();
  state.node_enabled = defaults.mesh.node_enabled ? "on" : "off";
  state.circuit_relay_enabled = defaults.mesh.capabilities.circuit_relay ? "on" : "off";
  state.media_relay_enabled = defaults.mesh.capabilities.media_relay ? "on" : "off";
  state.dht_enabled = defaults.mesh.capabilities.dht ? "on" : "off";
  state.prefer_contacts_for_routing = defaults.mesh.prefer_contacts_for_routing ? "on" : "off";
  state.direct_connections = mesh::DirectAudienceName(defaults.mesh.direct_connections);
  state.trusted_relays_only = defaults.mesh.trusted_relays_only ? "on" : "off";
  state.reachable_without_relay = defaults.mesh.reachable_without_relay ? "on" : "off";
  state.show_node_toggle = Platform::IsDesktop();
  state.mesh_status_message.clear();
}

} // namespace pbr
