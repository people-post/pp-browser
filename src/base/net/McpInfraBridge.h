#pragma once

#include "base/ai/mcp/McpClient.h"
#include "common/Error.h"

#include <nlohmann/json.hpp>

namespace pbr {

Roe<nlohmann::json> ParseMcpToolJsonResult(const nlohmann::json& tool_result);
Roe<nlohmann::json> CallMcpToolJson(McpClient& client, const std::string& tool_name, const nlohmann::json& arguments);

bool PromotedMcpInfraAvailable(McpClient* client);

} // namespace pbr
