#include "feature/settings/IntegrationsSettingsSection.h"

#include "foundation/data/Config.h"
#include "foundation/data/SessionStore.h"
#include "foundation/i18n/LocalizationService.h"
#include "feature/settings/SettingsLogic.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

bool McpServersEqual(const std::vector<McpServerUiState>& left, const std::vector<McpConfig>& right) {
  std::vector<McpServerUiState> normalized;
  for (const McpConfig& entry : right) {
    if (!entry.IsConfigured()) {
      continue;
    }
    normalized.push_back({.id = entry.id,
                            .url = entry.url,
                            .command = entry.command,
                            .args_text = JoinArgsText(entry.args),
                            .enabled = entry.enabled});
  }
  if (left.size() != normalized.size()) {
    return false;
  }
  for (size_t i = 0; i < left.size(); ++i) {
    if (left[i].id != normalized[i].id || left[i].url != normalized[i].url ||
        left[i].command != normalized[i].command || left[i].args_text != normalized[i].args_text ||
        left[i].enabled != normalized[i].enabled) {
      return false;
    }
  }
  return true;
}

} // namespace

const char* IntegrationsSettingsSection::Id() const {
  return "integrations";
}

SettingsSectionListItem IntegrationsSettingsSection::ListItem() const {
  return {.id = Id(),
          .title = Tr("settings.integrations.title"),
          .subtitle = Tr("settings.integrations.subtitle")};
}

SettingsFlushMode IntegrationsSettingsSection::FlushMode() const {
  return SettingsFlushMode::Debounced;
}

void IntegrationsSettingsSection::SyncFromSession(const BootstrapResult& bootstrap, SettingsUiState& state) {
  const AppConfig& config = bootstrap.config;
  state.promoted_mcp_url = config.promoted_mcp.url;
  state.search_provider = config.search.provider;
  state.mcp_servers.clear();
  for (const McpConfig& entry : config.mcp_servers) {
    state.mcp_servers.push_back({.id = entry.id,
                                 .url = entry.url,
                                 .command = entry.command,
                                 .args_text = JoinArgsText(entry.args),
                                 .enabled = entry.enabled});
  }
}

bool IntegrationsSettingsSection::IsPersisted(const SettingsUiState& state, const BootstrapResult& bootstrap) const {
  const AppConfig& config = bootstrap.config;
  return state.promoted_mcp_url == config.promoted_mcp.url && state.search_provider == config.search.provider &&
         McpServersEqual(state.mcp_servers, config.mcp_servers);
}

Roe<void> IntegrationsSettingsSection::Flush(SettingsUiState& state, SessionStore& store) {
  AppConfig config = ApplyIntegrationsSettingsDraft(store.Snapshot().config, state);
  if (auto saved = store.SaveConfig(config); !saved) {
    return saved.error();
  }
  SyncFromSession(store.Snapshot(), state);
  return {};
}

void IntegrationsSettingsSection::ResetToDefaults(SettingsUiState& state, const SessionStore& store) {
  state.promoted_mcp_url.clear();
  state.search_provider = store.DefaultConfig().search.provider;
  state.mcp_servers.clear();
}

} // namespace pbr
