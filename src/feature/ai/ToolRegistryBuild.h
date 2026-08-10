#pragma once

#include "base/ai/ToolRegistry.h"
#include "base/ai/mcp/McpClient.h"
#include "base/data/Config.h"

#include <string>
#include <vector>

namespace pbr {

// Clears and fills registry with web_search + MCP tools from AppConfig.
void BuildToolRegistryFromConfig(ToolRegistry& registry, const AppConfig& config, McpClient* promoted_mcp,
                                 const std::vector<McpClient*>& custom_mcps = {},
                                 const std::vector<std::string>& custom_prefixes = {});

} // namespace pbr
