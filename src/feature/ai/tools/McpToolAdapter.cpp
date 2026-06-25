#include "feature/ai/tools/McpToolAdapter.h"

#include "base/ai/LlmClient.h"

namespace pbr {

namespace {

bool RegistryHasTool(const ToolRegistry& registry, const std::string& name) {
  for (const ToolDescriptor& tool : registry.Tools()) {
    if (tool.definition.name == name) {
      return true;
    }
  }
  return false;
}

std::string ResolveToolName(const ToolRegistry& registry, const std::string& original,
                            const McpToolAdapterOptions& options) {
  if (options.denylist.contains(original)) {
    return {};
  }

  std::string name = original;
  if (!options.tool_prefix.empty()) {
    name = options.tool_prefix + original;
  }
  if (RegistryHasTool(registry, name)) {
    return {};
  }
  return name;
}

} // namespace

void McpToolAdapter::RegisterTools(ToolRegistry& registry, McpClient& client, const McpToolAdapterOptions& options) {
  auto tools = client.ListTools();
  if (!tools) {
    return;
  }

  for (const McpTool& mcp_tool : *tools) {
    const std::string tool_name = ResolveToolName(registry, mcp_tool.name, options);
    if (tool_name.empty()) {
      continue;
    }

    ToolDescriptor descriptor;
    descriptor.definition = ToolDefinition{
        .name = tool_name,
        .description = mcp_tool.description,
        .parameters = mcp_tool.input_schema,
    };
    descriptor.execute = [&client, name = mcp_tool.name](const nlohmann::json& arguments) -> Roe<std::string> {
      auto result = client.CallTool(name, arguments);
      if (!result) {
        return result.error();
      }
      return result->dump();
    };
    registry.Register(std::move(descriptor));
  }
}

} // namespace pbr
