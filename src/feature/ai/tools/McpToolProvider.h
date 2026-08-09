#pragma once

#include "feature/ai/IToolProvider.h"
#include "feature/ai/tools/McpToolAdapter.h"

#include <string>

namespace pbr {

class McpClient;

// Lists MCP tools as ToolDescriptors (same naming/prefix rules as McpToolAdapter).
class McpToolProvider : public IToolProvider {
public:
  McpToolProvider(std::string id, McpClient& client, McpToolAdapterOptions options = {},
                  std::string default_domain = "feeds");

  std::string Id() const override;
  std::vector<ToolDescriptor> ListTools() override;

private:
  std::string id_;
  McpClient& client_;
  McpToolAdapterOptions options_;
  std::string default_domain_;
};

} // namespace pbr
