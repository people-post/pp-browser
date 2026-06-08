#include "mcp/SchemaAdapter.h"

#include <sstream>

namespace ppbrowser {

std::string SchemaAdapter::ToolsToPromptContext(const std::vector<McpTool>& tools) {
  std::ostringstream out;
  for (const auto& tool : tools) {
    out << "- " << tool.name << ": " << tool.description << "\n";
    out << "  inputSchema: " << tool.input_schema.dump() << "\n";
  }
  return out.str();
}

nlohmann::json SchemaAdapter::ToolResultToRows(const nlohmann::json& tool_result) {
  if (!tool_result.contains("content")) {
    return nlohmann::json::array();
  }
  for (const auto& block : tool_result["content"]) {
    if (block.value("type", "") == "text") {
      const auto text = block.value("text", "[]");
      try {
        return nlohmann::json::parse(text);
      } catch (...) {
        return nlohmann::json::array();
      }
    }
  }
  return nlohmann::json::array();
}

std::string SchemaAdapter::RiskClass(const McpTool& tool) {
  const auto name = tool.name;
  if (name.find("delete") != std::string::npos || name.find("remove") != std::string::npos) {
    return "destructive";
  }
  if (name.find("create") != std::string::npos || name.find("update") != std::string::npos ||
      name.find("write") != std::string::npos) {
    return "write";
  }
  return "read";
}

} // namespace ppbrowser
