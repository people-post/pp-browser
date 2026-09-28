#include "domain/ai/mcp/SchemaAdapter.h"

#include "common/ValueJson.h"

#include <sstream>
#include "common/PbrCompat.h"

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
  // Name-based destructive detection is a floor, not a ceiling: a server cannot claim
  // read-only-ness away from an obviously destructive-sounding tool.
  if (name.find("delete") != std::string::npos || name.find("remove") != std::string::npos) {
    return "destructive";
  }
  if (tool.annotations.destructive_hint.value_or(false)) {
    return "destructive";
  }
  // MCP annotations are untrusted hints, but the only safe default is to require confirmation:
  // only an explicit readOnlyHint=true downgrades a tool to "read"; an unannotated tool (or one
  // that explicitly sets readOnlyHint=false) is always treated as "write".
  if (tool.annotations.read_only_hint.value_or(false)) {
    return "read";
  }
  return "write";
}

} // namespace pbr
