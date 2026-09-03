#pragma once

#include "foundation/data/Config.h"
#include "feature/settings/SettingsUiState.h"

#include <string>
#include <vector>

namespace pbr {

struct SettingsDraft {
  std::string llm_preset;
  std::string llm_base_url;
  std::string llm_model;
  std::string llm_api_key;
  std::string llm_api_key_env;
};

AppConfig ApplyLlmSettingsDraft(const AppConfig& base, const SettingsDraft& draft);
AppConfig ApplyIntegrationsSettingsDraft(const AppConfig& base, const SettingsUiState& state);
AppConfig ApplyNetworkSettingsDraft(const AppConfig& base, const SettingsUiState& state);

std::vector<std::string> ParseArgsText(const std::string& args_text);
std::string JoinArgsText(const std::vector<std::string>& args);

} // namespace pbr
