#pragma once

#include "domain/ai/IToolProvider.h"
#include "domain/ai/tools/McpToolAdapter.h"

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
  /** Whether the last ListTools() got an answer from the server (an empty list or an error status counts). */
  bool Listed() const { return listed_; }

private:
  std::string id_;
  McpClient& client_;
  McpToolAdapterOptions options_;
  std::string default_domain_;
  bool listed_ = false;
};

} // namespace pbr
