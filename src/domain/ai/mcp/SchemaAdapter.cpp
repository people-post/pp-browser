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
  // Explicit destructiveHint=true always wins, even over a (contradictory) readOnlyHint=true.
  if (tool.annotations.destructive_hint.has_value() && *tool.annotations.destructive_hint) {
    return "destructive";
  }
  if (tool.annotations.read_only_hint.value_or(false)) {
    return "read";
  }
  // Not read-only. Per the MCP annotations spec, destructiveHint defaults to true when
  // omitted; only an explicit destructiveHint=false downgrades to "write".
  if (tool.annotations.destructive_hint.value_or(true)) {
    return "destructive";
  }
  return "write";
}

} // namespace pbr
