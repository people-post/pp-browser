#include "lib/ai/tools/ToolRegistryBuild.h"

#include "lib/ai/tools/McpToolProvider.h"
#include "lib/ai/tools/WebSearchProvider.h"

#include <algorithm>

namespace pbr {

namespace {

// The promoted listing, dropped when the turn that asked for it was stopped while tools/list ran.
class StoppablePromotedProvider : public McpToolProvider {
public:
  StoppablePromotedProvider(McpClient& client, const long list_timeout_s, const std::atomic<bool>* cancel)
      : McpToolProvider("mcp:promoted", client, {.trust_server_annotations = true, .list_timeout_s = list_timeout_s},
                        "feeds"),
        cancel_(cancel) {}

  std::vector<ToolDescriptor> ListTools() override {
    std::vector<ToolDescriptor> tools = McpToolProvider::ListTools();
    if (cancel_ && cancel_->load()) {
      return {};
    }
    return tools;
  }

private:
  const std::atomic<bool>* cancel_;
};

} // namespace

bool RegisterPromotedMcpTools(ToolRegistry& registry, McpClient& promoted_mcp, const long list_timeout_s,
                              const std::atomic<bool>* cancel) {
  StoppablePromotedProvider promoted(promoted_mcp, list_timeout_s, cancel);
  registry.RegisterProvider(promoted);
  return promoted.Listed();
}

bool RegisterMcpServerTools(ToolRegistry& registry, McpClient* promoted_mcp, const std::vector<McpClient*>& custom_mcps,
                            const std::vector<std::string>& custom_prefixes, const long list_timeout_s) {
  bool all_listed = true;
  const auto count_of = [&registry](const std::string& provider) {
    return std::count_if(registry.Tools().begin(), registry.Tools().end(),
                         [&provider](const ToolDescriptor& tool) { return tool.meta.provider == provider; });
  };
  if (promoted_mcp && promoted_mcp->IsRunning()) {
    RegisterPromotedMcpTools(registry, *promoted_mcp, list_timeout_s);
    all_listed = all_listed && count_of("mcp:promoted") > 0;
  }

  for (size_t i = 0; i < custom_mcps.size(); ++i) {
    McpClient* client = custom_mcps[i];
    if (!client || !client->IsRunning()) {
      continue;
    }
    const std::string prefix =
        i < custom_prefixes.size() && !custom_prefixes[i].empty() ? custom_prefixes[i] + "__" : "";
    const std::string id = prefix.empty() ? ("mcp:custom_" + std::to_string(i)) : ("mcp:" + custom_prefixes[i]);
    McpToolProvider custom(id, *client, {.tool_prefix = prefix, .list_timeout_s = list_timeout_s}, "feeds");
    registry.RegisterProvider(custom);
    all_listed = all_listed && count_of(id) > 0;
  }
  return all_listed;
}

void BuildToolRegistryFromConfig(ToolRegistry& registry, const AppConfig& config, McpClient* promoted_mcp,
                                 const std::vector<McpClient*>& custom_mcps,
                                 const std::vector<std::string>& custom_prefixes) {
  registry.Clear();

  WebSearchProvider web_search(config.search);
  registry.RegisterProvider(web_search);

  RegisterMcpServerTools(registry, promoted_mcp, custom_mcps, custom_prefixes);
}

} // namespace pbr
