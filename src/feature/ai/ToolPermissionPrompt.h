#pragma once

#include "base/ai/TurnPlan.h"

#include <string>
#include <vector>

namespace pbr {

struct ParkedApproval;

/** Build assistant blocks JSON: short explanation + choice (Allow once / Always / Deny). */
std::string BuildToolPermissionChoiceBlocks(const std::string& approval_id,
                                            const std::vector<PlannedToolCall>& offered_tools);

std::string BuildToolPermissionDeniedBlocks(const std::vector<PlannedToolCall>& offered_tools);
std::string BuildToolPermissionStaleBlocks(const std::string& reason_code);

} // namespace pbr
