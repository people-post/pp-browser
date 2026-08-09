#include "feature/ai/tools/McpToolProvider.h"

#include "base/ai/mcp/McpClient.h"

namespace pbr {

McpToolProvider::McpToolProvider(std::string id, McpClient& client, McpToolAdapterOptions options,
                                 std::string default_domain)
    : id_(std::move(id)), client_(client), options_(std::move(options)),
      default_domain_(std::move(default_domain)) {}

std::string McpToolProvider::Id() const {
  return id_;
}

std::vector<ToolDescriptor> McpToolProvider::ListTools() {
  return McpToolAdapter::ListTools(client_, options_, {}, id_, default_domain_);
}

} // namespace pbr
