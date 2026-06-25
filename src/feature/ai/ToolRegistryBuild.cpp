#include "feature/ai/ToolRegistry.h"

#include "feature/ai/tools/McpToolAdapter.h"
#include "feature/ai/tools/MessagingTools.h"
#include "feature/ai/tools/WebSearchTool.h"
#include "base/ai/mcp/McpClient.h"
#include "feature/messaging/MessagingHub.h"

namespace pbr {

ToolRegistry ToolRegistry::BuildFromConfig(const AppConfig& config, McpClient* mcp_client) {
  ToolRegistry registry;
  registry.Register(WebSearchTool::Make(config.search));
  RegisterMessagingTools(registry, MessagingHub::Instance());

  if (mcp_client && mcp_client->IsRunning()) {
    McpToolAdapter::RegisterTools(registry, *mcp_client);
  }

  return registry;
}

} // namespace pbr
