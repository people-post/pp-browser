#pragma once

#include "base/data/Config.h"
#include "base/data/UserPreferences.h"
#include "common/Value.h"
#include "common/PbrCompat.h"

namespace pbr {

pp::common::Object LlmConfigToObject(const LlmConfig& config);
void LlmConfigFromObject(const pp::common::Object& object, LlmConfig& config);

pp::common::Object ContextBudgetToObject(const ContextBudget& budget);
void ContextBudgetFromObject(const pp::common::Object& object, ContextBudget& budget);

pp::common::Object SearchConfigToObject(const SearchConfig& config);
void SearchConfigFromObject(const pp::common::Object& object, SearchConfig& config);

pp::common::Object ServiceEndpointToObject(const ServiceEndpointConfig& endpoint);
void ServiceEndpointFromObject(const pp::common::Object& object,
                               ServiceEndpointConfig& endpoint);

pp::common::Object DirectoryConfigToObject(const DirectoryConfig& config);
void DirectoryConfigFromObject(const pp::common::Object& object, DirectoryConfig& config);

pp::common::Object MeshCapabilitiesToObject(const MeshCapabilities& caps);
void MeshCapabilitiesFromObject(const pp::common::Object& object,
                                  MeshCapabilities& caps);

pp::common::Object MeshDhtConfigToObject(const MeshDhtConfig& config);
void MeshDhtConfigFromObject(const pp::common::Object& object, MeshDhtConfig& config);

pp::common::Object MeshConfigToObject(const MeshConfig& config);
void MeshConfigFromObject(const pp::common::Object& object, MeshConfig& config);

pp::common::Object McpConfigToObject(const McpConfig& config);
void McpConfigFromObject(const pp::common::Object& object, McpConfig& config);

pp::common::Object AppConfigToObject(const AppConfig& config);
void AppConfigFromObject(const pp::common::Object& object, AppConfig& config);

pp::common::Object MachinePrefsToObject(const MachinePreferences& prefs);
void MachinePrefsFromObject(const pp::common::Object& object, MachinePreferences& prefs);

pp::common::Object ProfilePrefsToObject(const ProfilePreferences& prefs);
void ProfilePrefsFromObject(const pp::common::Object& object, ProfilePreferences& prefs);

AppConfig MergeConfig(const AppConfig& defaults, const pp::common::Object& overlay);

void ResolveConfigCredentials(AppConfig& config);

pp::common::Object ConfigToObject(const AppConfig& config, int config_version);

} // namespace pbr
