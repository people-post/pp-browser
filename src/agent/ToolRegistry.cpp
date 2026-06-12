#include "agent/ToolRegistry.h"

#include "agent/tools/McpToolAdapter.h"
#include "agent/tools/WebSearchTool.h"
#include "log/Logger.h"
#include "mcp/McpClient.h"

#include <sstream>

namespace pbr {

ToolRegistry::ToolRegistry() = default;

void ToolRegistry::Register(ToolDescriptor tool) {
  for (const ToolDescriptor& existing : tools_) {
    if (existing.definition.name == tool.definition.name) {
      logging::getLogger("ToolRegistry").warning << "Replacing duplicate tool: " << tool.definition.name;
    }
  }
  tools_.push_back(std::move(tool));
}

void ToolRegistry::Clear() {
  tools_.clear();
}

std::vector<ToolDefinition> ToolRegistry::Definitions() const {
  std::vector<ToolDefinition> out;
  out.reserve(tools_.size());
  for (const ToolDescriptor& tool : tools_) {
    out.push_back(tool.definition);
  }
  return out;
}

std::string ToolRegistry::SummaryForPrompt() const {
  std::ostringstream out;
  for (const ToolDescriptor& tool : tools_) {
    out << "- " << tool.definition.name << ": " << tool.definition.description << "\n";
  }
  return out.str();
}

Roe<std::string> ToolRegistry::Execute(const std::string& name, const nlohmann::json& arguments) const {
  for (const ToolDescriptor& tool : tools_) {
    if (tool.definition.name == name) {
      return tool.execute(arguments);
    }
  }
  return Error("Unknown tool: " + name);
}

ToolRegistry ToolRegistry::BuildFromConfig(const AppConfig& config, McpClient* mcp_client) {
  ToolRegistry registry;
  registry.Register(WebSearchTool::Make(config.search));

  if (mcp_client && mcp_client->IsRunning()) {
    McpToolAdapter::RegisterTools(registry, *mcp_client);
  }

  return registry;
}

} // namespace pbr
