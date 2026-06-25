#pragma once

#include "base/ai/TurnPlan.h"

#include <optional>
#include <string>

namespace pbr {

std::optional<TurnPlan> TryBuildPlanFromPayload(const std::string& user_text, const std::string& payload);

} // namespace pbr
