#include "base/net/McpInfraBridge.h"

namespace pbr {

namespace {

Roe<nlohmann::json> ParseJsonText(const std::string& text) {
  const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
  if (parsed.is_discarded()) {
    return Error("Failed to parse MCP tool result text as JSON");
  }
  return parsed;
}

} // namespace

Roe<nlohmann::json> ParseMcpToolJsonResult(const nlohmann::json& tool_result) {
  if (tool_result.is_object() && tool_result.contains("messages") && tool_result["messages"].is_array()) {
    return tool_result;
  }
  if (tool_result.is_array()) {
    return tool_result;
  }
  if (tool_result.is_object() && tool_result.contains("success")) {
    return tool_result;
  }
  if (tool_result.is_object() && tool_result.contains("hits") && tool_result["hits"].is_array()) {
    return tool_result;
  }

  if (!tool_result.contains("content") || !tool_result["content"].is_array()) {
    if (tool_result.is_object()) {
      return tool_result;
    }
    return Error("MCP tool result missing content");
  }

  for (const auto& block : tool_result["content"]) {
    if (block.value("type", "") != "text") {
      continue;
    }
    return ParseJsonText(block.value("text", "{}"));
  }

  return Error("MCP tool result has no text content");
}

Roe<nlohmann::json> CallMcpToolJson(McpClient& client, const std::string& tool_name,
                                    const nlohmann::json& arguments) {
  auto result = client.CallTool(tool_name, arguments);
  if (!result) {
    return result.error();
  }
  return ParseMcpToolJsonResult(*result);
}

bool PromotedMcpInfraAvailable(McpClient* client) {
  return client != nullptr && client->IsRunning();
}

} // namespace pbr
