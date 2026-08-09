#pragma once

#include "feature/ai/ToolRegistry.h"
#include "base/ai/mcp/McpClient.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace pbr {

struct McpToolAdapterOptions {
  std::unordered_set<std::string> denylist;
  std::string tool_prefix;
};

class McpToolAdapter {
public:
  // Build descriptors for tools currently advertised by the client.
  // `occupied_names` skips collisions (e.g. tools already in the registry).
  static std::vector<ToolDescriptor> ListTools(McpClient& client, const McpToolAdapterOptions& options = {},
                                               const std::unordered_set<std::string>& occupied_names = {},
                                               const std::string& provider_id = {},
                                               const std::string& default_domain = "feeds");

  static void RegisterTools(ToolRegistry& registry, McpClient& client,
                            const McpToolAdapterOptions& options = {});
};

} // namespace pbr
