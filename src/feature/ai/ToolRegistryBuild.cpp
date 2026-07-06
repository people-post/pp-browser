#include "feature/ai/ToolRegistry.h"

#include "feature/ai/tools/McpToolAdapter.h"
#include "feature/ai/tools/MessagingTools.h"
#include "feature/ai/tools/WebSearchTool.h"
#include "base/ai/mcp/McpClient.h"
#include "feature/messaging/MessagingHub.h"

namespace pbr {

ToolRegistry ToolRegistry::BuildFromConfig(const AppConfig& config, McpClient* promoted_mcp,
                                           const std::vector<McpClient*>& custom_mcps,
                                           const std::vector<std::string>& custom_prefixes) {
  ToolRegistry registry;
  registry.Register(WebSearchTool::Make(config.search));
  RegisterMessagingTools(registry, MessagingHub::Instance());

  if (promoted_mcp && promoted_mcp->IsRunning()) {
    McpToolAdapter::RegisterTools(registry, *promoted_mcp, {});
  }

  for (size_t i = 0; i < custom_mcps.size(); ++i) {
    McpClient* client = custom_mcps[i];
    if (!client || !client->IsRunning()) {
      continue;
    }
    const std::string prefix = i < custom_prefixes.size() && !custom_prefixes[i].empty()
                                   ? custom_prefixes[i] + "__"
                                   : "";
    McpToolAdapter::RegisterTools(registry, *client, {.tool_prefix = prefix});
  }

  return registry;
}

} // namespace pbr
