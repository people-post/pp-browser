#include "feature/settings/SettingsLogic.h"

#include "common/ArgsText.h"
#include "foundation/data/Config.h"
#include "foundation/data/MeshRole.h"
#include "foundation/data/LlmPreset.h"
#include "foundation/platform/DeploymentProfile.h"
#include "feature/settings/SettingsUiState.h"

#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

AppConfig ApplyLlmSettingsDraft(const AppConfig& base, const SettingsDraft& draft) {
  AppConfig config = base;

  ApplyPreset(config, draft.llm_preset, draft.llm_base_url);
  config.llm.model = draft.llm_model;

  if (!draft.llm_api_key.empty()) {
    config.llm.api_key = draft.llm_api_key;
    config.llm_api_key_env.clear();
  } else if (!draft.llm_api_key_env.empty()) {
    config.llm_api_key_env = draft.llm_api_key_env;
    config.llm.api_key.clear();
  } else if (ResolvePreset(config) == "brief" || ResolvePreset(config) == "ollama") {
    config.llm.api_key.clear();
    config.llm_api_key_env.clear();
  }

  NormalizeLlmConfig(config);
  return config;
}

AppConfig ApplyIntegrationsSettingsDraft(const AppConfig& base, const SettingsUiState& state) {
  AppConfig config = base;
  config.promoted_mcp.url = state.promoted_mcp_url;
  config.promoted_mcp.command.clear();
  config.promoted_mcp.args.clear();
  config.search.provider = state.search_provider.empty() ? config.search.provider : state.search_provider;

  config.mcp_servers.clear();
  int auto_id = 1;
  for (const McpServerUiState& row : state.mcp_servers) {
    if (row.url.empty() && row.command.empty()) {
      continue;
    }
    McpConfig entry;
    entry.id = row.id.empty() ? ("custom-" + std::to_string(auto_id++)) : row.id;
    entry.url = row.url;
    entry.command = row.command;
    entry.args = ParseArgsText(row.args_text);
    entry.enabled = row.enabled;
    config.mcp_servers.push_back(std::move(entry));
  }
  return config;
}

std::string EndpointLinkText(const ServiceEndpointConfig& endpoint) {
  return endpoint.peer_id.empty() ? endpoint.base_url : endpoint.base_url + "#" + endpoint.peer_id;
}

void SetEndpointFromLinkText(ServiceEndpointConfig& endpoint, const std::string& text) {
  const size_t hash = text.find('#');
  endpoint.base_url = text.substr(0, hash);
  endpoint.peer_id = hash == std::string::npos ? std::string() : text.substr(hash + 1);
}

std::string MissingRelayIdSetting(const AppConfig& config, const bool developer_build, const bool compiled_backend) {
  if (OrgRelayFirstUseAllowed(developer_build, compiled_backend)) {
    return {};
  }
  const auto trimmed = [](std::string url) {
    while (!url.empty() && url.back() == '/') {
      url.pop_back();
    }
    return url;
  };
  if (!config.relay.base_url.empty() && config.relay.peer_id.empty()) {
    return "relay.peer_id";
  }
  const bool apart = trimmed(config.registration.base_url) != trimmed(config.relay.base_url);
  if (apart && !config.registration.base_url.empty() && config.registration.peer_id.empty()) {
    return "registration.peer_id";
  }
  return {};
}

AppConfig ApplyNetworkSettingsDraft(const AppConfig& base, const SettingsUiState& state) {
  AppConfig config = base;
  const AppConfig defaults = Config::DefaultAppConfig();
  if (!state.backend_env.empty()) {
    config.backend.env = BackendEnvName(ParseBackendEnv(state.backend_env));
  }
  // The URL fields are the custom backend's; prod / staging / dev derive theirs from the origin and leave them alone.
  if (state.backend_env.empty() || state.backend_env == "custom") {
    // When an origin replaced the loaded URLs, the saved copies are what the fields show and what a save writes.
    ServiceEndpointConfig& relay = config.backend.has_saved_urls ? config.backend.saved_relay : config.relay;
    DirectoryConfig& directory = config.backend.has_saved_urls ? config.backend.saved_directory : config.directory;
    ServiceEndpointConfig& registration =
        config.backend.has_saved_urls ? config.backend.saved_registration : config.registration;
    // The relay fields take relay links (MR008: the relay's address with its PeerId).
    SetEndpointFromLinkText(relay,
                            state.relay_base_url.empty() ? EndpointLinkText(defaults.relay) : state.relay_base_url);
    directory.base_url = state.directory_base_url.empty() ? defaults.directory.base_url : state.directory_base_url;
    // Settings UI edits the primary URL only; clear providers so EffectiveDirectoryProviders
    // follows base_url (N029 nd3). Advanced providers[] remain config.json / ops.
    directory.providers.clear();
    directory.transport = "http";
    SetEndpointFromLinkText(registration, state.registration_base_url.empty() ? EndpointLinkText(defaults.registration)
                                                                              : state.registration_base_url);
  }
  config.mesh.node_enabled = (state.node_enabled != "off");
  config.mesh.capabilities.circuit_relay = (state.circuit_relay_enabled == "on");
  config.mesh.capabilities.media_relay = (state.media_relay_enabled == "on");
  config.mesh.capabilities.dht = (state.dht_enabled == "on");
  config.mesh.prefer_contacts_for_routing = (state.prefer_contacts_for_routing != "off");
  config.mesh.trusted_relays_only = (state.trusted_relays_only == "on");
  config.mesh.reachable_without_relay = (state.reachable_without_relay == "on");
  if (auto audience = mesh::DirectAudienceFromName(state.direct_connections)) {
    config.mesh.direct_connections = *audience;
  }
  NormalizeMeshConfig(config.mesh);
  return config;
}

} // namespace pbr
