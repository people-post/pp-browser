#pragma once

#include "domain/ai/ToolRegistry.h"
#include "domain/ai/mcp/McpClient.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace pbr {

struct McpToolAdapterOptions {
  std::unordered_set<std::string> denylist;
  std::string tool_prefix;
  // Whether the server's own `readOnlyHint` may make a tool "read" (runs without asking). True only for the
  // promoted server, which is ours; a custom server's tool is at least "write" (PB-055).
  bool trust_server_annotations = false;
  // tools/list request bound in seconds; 0 = the transport's default.
  long list_timeout_s = 0;
};

class McpToolAdapter {
public:
  // Build descriptors for tools currently advertised by the client.
  // `occupied_names` skips collisions (e.g. tools already in the registry).
  // `listed` (optional): whether tools/list answered; a failed listing returns no tools.
  static std::vector<ToolDescriptor> ListTools(McpClient& client, const McpToolAdapterOptions& options = {},
                                               const std::unordered_set<std::string>& occupied_names = {},
                                               const std::string& provider_id = {},
                                               const std::string& default_domain = "feeds",
                                               bool* listed = nullptr);

  static void RegisterTools(ToolRegistry& registry, McpClient& client,
                            const McpToolAdapterOptions& options = {});
};

} // namespace pbr
