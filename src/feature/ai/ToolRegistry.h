#pragma once

#include "base/ai/LlmClient.h"
#include "base/data/Config.h"
#include "common/Error.h"

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace pbr {

class McpClient;

struct ToolDescriptor {
  ToolDefinition definition;
  std::function<Roe<std::string>(const nlohmann::json& arguments)> execute;
};

class ToolRegistry {
public:
  ToolRegistry();

  void Register(ToolDescriptor tool);
  void Clear();

  const std::vector<ToolDescriptor>& Tools() const { return tools_; }
  std::vector<ToolDefinition> Definitions() const;
  std::string SummaryForPrompt() const;

  Roe<std::string> Execute(const std::string& name, const nlohmann::json& arguments) const;

  static ToolRegistry BuildFromConfig(const AppConfig& config, McpClient* promoted_mcp,
                                      const std::vector<McpClient*>& custom_mcps = {},
                                      const std::vector<std::string>& custom_prefixes = {});

private:
  std::vector<ToolDescriptor> tools_;
};

} // namespace pbr
