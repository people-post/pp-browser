#pragma once

#include "feature/ai/ToolRegistry.h"
#include "base/ai/mcp/McpClient.h"

#include <string>
#include <unordered_set>

namespace pbr {

struct McpToolAdapterOptions {
  std::unordered_set<std::string> denylist;
  std::string tool_prefix;
};

class McpToolAdapter {
public:
  static void RegisterTools(ToolRegistry& registry, McpClient& client,
                            const McpToolAdapterOptions& options = {});
};

} // namespace pbr
