#include "feature/ai/tools/McpToolAdapter.h"

#include "base/ai/LlmClient.h"

namespace pbr {

void McpToolAdapter::RegisterTools(ToolRegistry& registry, McpClient& client) {
  auto tools = client.ListTools();
  if (!tools) {
    return;
  }

  for (const McpTool& mcp_tool : *tools) {
    ToolDescriptor descriptor;
    descriptor.definition = ToolDefinition{
        .name = mcp_tool.name,
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
