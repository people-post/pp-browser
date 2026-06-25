#pragma once

#include "base/data/Config.h"

#include <string>

namespace pbr {

std::string ResolvePreset(const AppConfig& config);

void ApplyPreset(AppConfig& config, const std::string& preset_id, const std::string& custom_base_url);

// Sets require_api_key from preset and whether credentials are configured.
void ResolveLlmAuthRequirements(AppConfig& config);

} // namespace pbr
