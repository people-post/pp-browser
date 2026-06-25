#include "agent/ToolRegistry.h"

#include "agent/tools/McpToolAdapter.h"
#include "agent/tools/MessagingTools.h"
#include "agent/tools/WebSearchTool.h"
#include "mcp/McpClient.h"
#include "messaging/MessagingHub.h"

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
