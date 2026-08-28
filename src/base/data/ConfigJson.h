#pragma once

#include "base/data/Config.h"
#include "base/data/UserPreferences.h"
#include "common/PbrCompat.h"

namespace pbr {

Object LlmConfigToObject(const LlmConfig& config);
void LlmConfigFromObject(const Object& object, LlmConfig& config);

Object ContextBudgetToObject(const ContextBudget& budget);
void ContextBudgetFromObject(const Object& object, ContextBudget& budget);

Object SearchConfigToObject(const SearchConfig& config);
void SearchConfigFromObject(const Object& object, SearchConfig& config);

Object ServiceEndpointToObject(const ServiceEndpointConfig& endpoint);
void ServiceEndpointFromObject(const Object& object, ServiceEndpointConfig& endpoint);

Object Libp2pCapabilitiesToObject(const Libp2pCapabilities& caps);
void Libp2pCapabilitiesFromObject(const Object& object, Libp2pCapabilities& caps);

Object Libp2pConfigToObject(const Libp2pConfig& config);
void Libp2pConfigFromObject(const Object& object, Libp2pConfig& config);

Object McpConfigToObject(const McpConfig& config);
void McpConfigFromObject(const Object& object, McpConfig& config);

Object AppConfigToObject(const AppConfig& config);
void AppConfigFromObject(const Object& object, AppConfig& config);

Object MachinePrefsToObject(const MachinePreferences& prefs);
void MachinePrefsFromObject(const Object& object, MachinePreferences& prefs);

Object ProfilePrefsToObject(const ProfilePreferences& prefs);
void ProfilePrefsFromObject(const Object& object, ProfilePreferences& prefs);

AppConfig MergeConfig(const AppConfig& defaults, const Object& overlay);

void ResolveConfigCredentials(AppConfig& config);

Object ConfigToObject(const AppConfig& config, int config_version);

} // namespace pbr
