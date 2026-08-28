#pragma once

#include "base/ai/LlmClient.h"
#include "common/Error.h"
#include "common/PbrCompat.h"

#include <functional>
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
  std::function<Roe<std::string>(const Object& arguments)> execute;
};

/**
 * Build a ToolDescriptor without putting the execute lambda inside a
 * designated-initializer brace list. MSVC misparses `.member` access inside
 * such lambdas (C3878: unexpected token '.').
 */
inline ToolDescriptor MakeTool(
    ToolDefinition definition, ToolMeta meta,
    std::function<Roe<std::string>(const Object& arguments)> execute) {
  ToolDescriptor tool;
  tool.definition = std::move(definition);
  tool.meta = std::move(meta);
  tool.execute = std::move(execute);
  return tool;
}

} // namespace pbr
