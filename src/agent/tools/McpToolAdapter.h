#pragma once

#include "agent/ToolRegistry.h"
#include "mcp/McpClient.h"

namespace pbr {

class McpToolAdapter {
public:
  static void RegisterTools(ToolRegistry& registry, McpClient& client);
};

} // namespace pbr
