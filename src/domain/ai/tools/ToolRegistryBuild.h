#pragma once

#include "domain/ai/ToolRegistry.h"
#include "domain/ai/mcp/McpClient.h"
#include "foundation/data/Config.h"

#include <atomic>
#include <string>
#include <vector>

namespace pbr {

// Clears and fills registry with web_search + MCP tools from AppConfig.
void BuildToolRegistryFromConfig(ToolRegistry& registry, const AppConfig& config, McpClient* promoted_mcp,
                                 const std::vector<McpClient*>& custom_mcps = {},
                                 const std::vector<std::string>& custom_prefixes = {});

// Adds the promoted MCP server's tools (the feeds); a name already in the registry is skipped.
// `list_timeout_s` bounds the tools/list request (0 = the transport's default). True when tools/list answered.
// `cancel`: when set by the time tools/list returns, nothing is registered.
bool RegisterPromotedMcpTools(ToolRegistry& registry, McpClient& promoted_mcp, long list_timeout_s = 0,
                              const std::atomic<bool>* cancel = nullptr);

} // namespace pbr
