#include "feature/ai/ToolRegistry.h"

#include "feature/ai/tools/McpToolAdapter.h"
#include "feature/ai/tools/WebSearchTool.h"
#include "base/ai/mcp/McpClient.h"

namespace pbr {

void ToolRegistry::BuildFromConfig(const AppConfig& config, McpClient* promoted_mcp,
                                   const std::vector<McpClient*>& custom_mcps,
                                   const std::vector<std::string>& custom_prefixes) {
  Clear();
  Register(WebSearchTool::Make(config.search));

  if (promoted_mcp && promoted_mcp->IsRunning()) {
    McpToolAdapter::RegisterTools(*this, *promoted_mcp, {});
  }

  for (size_t i = 0; i < custom_mcps.size(); ++i) {
    McpClient* client = custom_mcps[i];
    if (!client || !client->IsRunning()) {
      continue;
    }
    const std::string prefix = i < custom_prefixes.size() && !custom_prefixes[i].empty()
                                   ? custom_prefixes[i] + "__"
                                   : "";
    McpToolAdapter::RegisterTools(*this, *client, {.tool_prefix = prefix});
  }
}

} // namespace pbr
