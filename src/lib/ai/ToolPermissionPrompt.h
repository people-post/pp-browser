#pragma once

#include "lib/ai/TurnPlan.h"

#include <string>
#include <vector>

namespace pbr {

struct ParkedApproval;

/** "name(key=value, …)" for each call, comma-separated: what the user is asked to approve (PB-015). */
std::string DescribeToolCalls(const std::vector<PlannedToolCall>& tools);

/**
 * Build assistant blocks JSON: short explanation + choice (Allow once / Always / Deny). The prompt names each
 * tool with the arguments it will run with. `offer_allow_always` is false when any offered tool must be
 * confirmed every time (ToolPermissionPolicy::AllowAlwaysEligible); the Always option is then left out.
 */
std::string BuildToolPermissionChoiceBlocks(const std::string& approval_id,
                                            const std::vector<PlannedToolCall>& offered_tools,
                                            bool offer_allow_always);

std::string BuildToolPermissionDeniedBlocks(const std::vector<PlannedToolCall>& offered_tools);
std::string BuildToolPermissionStaleBlocks(const std::string& reason_code);

} // namespace pbr
