#pragma once

#include "feature/ai/ToolRegistry.h"
#include "base/ai/mcp/McpClient.h"

namespace pbr {

class McpToolAdapter {
public:
  static void RegisterTools(ToolRegistry& registry, McpClient& client);
};

} // namespace pbr
