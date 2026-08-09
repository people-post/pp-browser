#pragma once

#include "base/ai/LlmClient.h"
#include "common/Error.h"

#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace pbr {

// Routing / policy metadata for planner prompts and future allowlists.
// Open domain labels align with projects/ai-centric-interface (domains are open).
struct ToolMeta {
  std::string provider; // e.g. "messaging", "web_search", "mcp:brief"
  std::string domain;   // e.g. "people", "knowledge", "feeds", "identity"
  std::string risk;     // "read" | "write" | "destructive"
  bool mutating = false;
};

struct ToolDescriptor {
  ToolDefinition definition;
  ToolMeta meta;
  std::function<Roe<std::string>(const nlohmann::json& arguments)> execute;
};

} // namespace pbr
