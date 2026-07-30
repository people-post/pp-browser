#include "feature/settings/SettingsLogic.h"

#include "base/data/Config.h"
#include "base/data/Libp2pRole.h"
#include "base/data/LlmPreset.h"
#include "feature/settings/SettingsUiState.h"

#include <sstream>

namespace pbr {

namespace {

std::vector<std::string> SplitArgs(const std::string& text) {
  std::vector<std::string> args;
  std::istringstream stream(text);
  std::string token;
  while (stream >> token) {
    args.push_back(token);
  }
  return args;
}

} // namespace

std::vector<std::string> ParseArgsText(const std::string& args_text) {
  return SplitArgs(args_text);
}

std::string JoinArgsText(const std::vector<std::string>& args) {
  std::ostringstream out;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i > 0) {
      out << ' ';
    }
    out << args[i];
  }
  return out.str();
}

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

AppConfig ApplyNetworkSettingsDraft(const AppConfig& base, const SettingsUiState& state) {
  AppConfig config = base;
  const AppConfig defaults = Config::DefaultAppConfig();
  config.relay.base_url =
      state.relay_base_url.empty() ? defaults.relay.base_url : state.relay_base_url;
  config.directory.base_url =
      state.directory_base_url.empty() ? defaults.directory.base_url : state.directory_base_url;
  config.registration.base_url = state.registration_base_url.empty()
                                     ? defaults.registration.base_url
                                     : state.registration_base_url;
  config.libp2p.node_enabled = (state.node_enabled != "off");
  config.libp2p.capabilities.circuit_relay = (state.circuit_relay_enabled == "on");
  config.libp2p.capabilities.media_relay = (state.media_relay_enabled == "on");
  config.libp2p.prefer_contacts_for_routing = (state.prefer_contacts_for_routing != "off");
  if (!state.libp2p_listen_multiaddr.empty()) {
    config.libp2p.listen_multiaddr = state.libp2p_listen_multiaddr;
  }
  NormalizeLibp2pConfig(config.libp2p);
  return config;
}

} // namespace pbr
