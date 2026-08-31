#pragma once

#include "common/Error.h"
#include "base/ai/mcp/McpClient.h"
#include "common/PbrCompat.h"

#include <string>
#include <vector>

namespace pbr {

class SchemaAdapter {
public:
  static std::string ToolsToPromptContext(const std::vector<McpTool>& tools);
  static Roe<Value> ToolResultToRows(const Object& tool_result);
  static std::string RiskClass(const McpTool& tool);
};

} // namespace pbr
