#pragma once

#include "base/ai/LlmClient.h"
#include "base/data/Config.h"
#include "common/Error.h"
#include "common/Module.h"

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace pbr {

class IToolProvider;
class McpClient;

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

class ToolRegistry : public Module {
public:
  ToolRegistry();

  void Register(ToolDescriptor tool);
  void RegisterProvider(IToolProvider& provider);
  void Clear();

  const std::vector<ToolDescriptor>& Tools() const { return tools_; }
  std::vector<ToolDefinition> Definitions() const;
  std::string SummaryForPrompt() const;

  Roe<std::string> Execute(const std::string& name, const nlohmann::json& arguments) const;

  void BuildFromConfig(const AppConfig& config, McpClient* promoted_mcp,
                       const std::vector<McpClient*>& custom_mcps = {},
                       const std::vector<std::string>& custom_prefixes = {});

private:
  std::vector<ToolDescriptor> tools_;
};

} // namespace pbr
