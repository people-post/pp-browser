#include "feature/ai/tools/McpToolAdapter.h"

#include "domain/ai/LlmClient.h"
#include "domain/ai/mcp/SchemaAdapter.h"
#include "common/PlatformLimits.h"
#include "common/ValueJson.h"

#include <unordered_set>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::unordered_set<std::string> RegistryToolNames(const ToolRegistry& registry) {
  std::unordered_set<std::string> names;
  for (const ToolDescriptor& tool : registry.Tools()) {
    names.insert(tool.definition.name);
  }
  return names;
}

std::string ResolveToolName(const std::unordered_set<std::string>& occupied, const std::string& original,
                            const McpToolAdapterOptions& options) {
  if (options.denylist.contains(original)) {
    return {};
  }

  std::string name = original;
  if (!options.tool_prefix.empty()) {
    name = options.tool_prefix + original;
  }
  if (occupied.contains(name)) {
    return {};
  }
  return name;
}

Roe<std::string> CallMcpToolJson(McpClient& client, const std::string& name, const Object& arguments) {
  auto result = client.CallTool(name, arguments);
  if (!result) {
    return result.error();
  }

  std::string result_json = DumpJson(*result);
  if (result_json.size() > kMaxMcpToolResultBytes) {
    return Error("MCP tool result exceeds limit of " + std::to_string(kMaxMcpToolResultBytes) + " bytes");
  }
  return result_json;
}

} // namespace

std::vector<ToolDescriptor> McpToolAdapter::ListTools(McpClient& client, const McpToolAdapterOptions& options,
                                                      const std::unordered_set<std::string>& occupied_names,
                                                      const std::string& provider_id,
                                                      const std::string& default_domain) {
  std::vector<ToolDescriptor> out;
  auto tools = client.ListTools();
  if (!tools) {
    return out;
  }

  std::unordered_set<std::string> occupied = occupied_names;
  for (const McpTool& mcp_tool : *tools) {
    const std::string tool_name = ResolveToolName(occupied, mcp_tool.name, options);
    if (tool_name.empty()) {
      continue;
    }
    occupied.insert(tool_name);

    const std::string risk = SchemaAdapter::RiskClass(mcp_tool);
    ToolDescriptor descriptor;
    descriptor.definition = ToolDefinition{
        .name = tool_name,
        .description = mcp_tool.description,
        .parameters = mcp_tool.input_schema,
    };
    descriptor.meta = ToolMeta{
        .provider = provider_id.empty() ? std::string("mcp") : provider_id,
        .domain = default_domain,
        .risk = risk,
        .mutating = risk != "read",
    };
    descriptor.execute = [&client, name = mcp_tool.name](const Object& arguments) -> Roe<std::string> {
      return CallMcpToolJson(client, name, arguments);
    };
    out.push_back(std::move(descriptor));
  }
  return out;
}

void McpToolAdapter::RegisterTools(ToolRegistry& registry, McpClient& client,
                                   const McpToolAdapterOptions& options) {
  for (ToolDescriptor& descriptor : ListTools(client, options, RegistryToolNames(registry))) {
    registry.Register(std::move(descriptor));
  }
}

} // namespace pbr
