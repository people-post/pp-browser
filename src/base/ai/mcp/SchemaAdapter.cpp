#include "base/ai/mcp/SchemaAdapter.h"

#include "common/ValueJson.h"

#include <sstream>

namespace pbr {

std::string SchemaAdapter::ToolsToPromptContext(const std::vector<McpTool>& tools) {
  std::ostringstream out;
  for (const auto& tool : tools) {
    out << "- " << tool.name << ": " << tool.description << "\n";
    out << "  inputSchema: " << DumpJson(tool.input_schema) << "\n";
  }
  return out.str();
}

Roe<Value> SchemaAdapter::ToolResultToRows(const Object& tool_result) {
  const Array* content = tool_result.getArray("content");
  if (!content) {
    return ArrayValue({});
  }
  for (const Value& block_value : content->elements) {
    const Object* block = asObject(block_value);
    if (!block) {
      continue;
    }
    if (block->getString("type").value_or("") == "text") {
      const std::string text = block->getString("text").value_or("[]");
      auto rows = ParseValue(text);
      if (!rows) {
        return Error("Failed to parse tool result text as JSON");
      }
      return *rows;
    }
  }
  return ArrayValue({});
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

} // namespace pbr
