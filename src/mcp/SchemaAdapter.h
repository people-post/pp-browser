#pragma once

#include "mcp/McpClient.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace pbr {

class SchemaAdapter {
public:
  static std::string ToolsToPromptContext(const std::vector<McpTool>& tools);
  static nlohmann::json ToolResultToRows(const nlohmann::json& tool_result);
  static std::string RiskClass(const McpTool& tool);
};

} // namespace pbr
