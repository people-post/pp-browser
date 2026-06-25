#pragma once

#include "feature/settings/SettingsSectionHandler.h"

#include <memory>
#include <vector>

namespace pbr {

std::vector<std::unique_ptr<SettingsSectionHandler>> CreateSettingsSections();

} // namespace pbr
