#pragma once

#include "base/data/Config.h"
#include "base/data/UserPreferences.h"

#include <nlohmann/json_fwd.hpp>
#include <string>

namespace pbr {

void to_json(nlohmann::json& j, const LlmConfig& config);
void from_json(const nlohmann::json& j, LlmConfig& config);

void to_json(nlohmann::json& j, const ContextBudget& budget);
void from_json(const nlohmann::json& j, ContextBudget& budget);

void to_json(nlohmann::json& j, const SearchConfig& config);
void from_json(const nlohmann::json& j, SearchConfig& config);

void to_json(nlohmann::json& j, const ServiceEndpointConfig& endpoint);
void from_json(const nlohmann::json& j, ServiceEndpointConfig& endpoint);

void to_json(nlohmann::json& j, const McpConfig& config);
void from_json(const nlohmann::json& j, McpConfig& config);

void to_json(nlohmann::json& j, const AppConfig& config);
void from_json(const nlohmann::json& j, AppConfig& config);

void to_json(nlohmann::json& j, const MachinePreferences& prefs);
void from_json(const nlohmann::json& j, MachinePreferences& prefs);

void to_json(nlohmann::json& j, const ProfilePreferences& prefs);
void from_json(const nlohmann::json& j, ProfilePreferences& prefs);

void DeepMergeJson(nlohmann::json& base, const nlohmann::json& overlay);

AppConfig MergeConfig(const AppConfig& defaults, const nlohmann::json& overlay);

void ResolveConfigCredentials(AppConfig& config);

nlohmann::json ConfigToJson(const AppConfig& config, int config_version);
nlohmann::json MachinePrefsToJson(const MachinePreferences& prefs);
nlohmann::json ProfilePrefsToJson(const ProfilePreferences& prefs);

} // namespace pbr
