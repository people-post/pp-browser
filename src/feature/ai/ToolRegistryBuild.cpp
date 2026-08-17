#include "feature/ai/ToolRegistryBuild.h"

#include "feature/ai/tools/McpToolProvider.h"
#include "feature/ai/tools/WebSearchProvider.h"

namespace pbr {

void BuildToolRegistryFromConfig(ToolRegistry& registry, const AppConfig& config, McpClient* promoted_mcp,
                                 const std::vector<McpClient*>& custom_mcps,
                                 const std::vector<std::string>& custom_prefixes) {
  registry.Clear();

  WebSearchProvider web_search(config.search);
  registry.RegisterProvider(web_search);

  if (promoted_mcp && promoted_mcp->IsRunning()) {
    McpToolProvider promoted("mcp:promoted", *promoted_mcp, {}, "feeds");
    registry.RegisterProvider(promoted);
  }

  for (size_t i = 0; i < custom_mcps.size(); ++i) {
    McpClient* client = custom_mcps[i];
    if (!client || !client->IsRunning()) {
      continue;
    }
    const std::string prefix = i < custom_prefixes.size() && !custom_prefixes[i].empty()
                                   ? custom_prefixes[i] + "__"
                                   : "";
    const std::string id = prefix.empty() ? ("mcp:custom_" + std::to_string(i)) : ("mcp:" + custom_prefixes[i]);
    McpToolProvider custom(id, *client, {.tool_prefix = prefix}, "feeds");
    registry.RegisterProvider(custom);
  }
}

} // namespace pbr
