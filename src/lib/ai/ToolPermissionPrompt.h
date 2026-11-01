#pragma once

#include "lib/ai/ParkedApproval.h"
#include "lib/ai/TurnPlan.h"

#include <optional>
#include <string>
#include <vector>

namespace pbr {

/**
 * What the user is asked to approve, in plain words (chat.permission.tool.<tool>, values from ai.option.*): one
 * phrase per call, joined with the locale's list separator. The raw tool name and argument names never appear;
 * a tool without copy reads chat.permission.tool.generic (PB-015).
 */
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

/**
 * The stored card (blocks JSON built by BuildToolPermissionChoiceBlocks) after it ended. Stale: the whole card
 * becomes the "no longer active" notice. Allowed / denied: the paragraph stays, the choice block is replaced by
 * one result line. nullopt when `blocks_json` is not a blocks document.
 */
std::optional<std::string> RewriteToolPermissionCardBlocks(const std::string& blocks_json, ApprovalOutcome outcome);

} // namespace pbr
