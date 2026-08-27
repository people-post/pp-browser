#pragma once

#include "base/ai/IToolProvider.h"
#include "base/ai/ToolDescriptor.h"
#include "base/ai/LlmClient.h"
#include "common/Error.h"
#include "common/Module.h"
#include "common/PbrCompat.h"

#include <string>
#include <vector>

namespace pbr {

// Catalog of agent tools. Feature providers register here;
// BuildToolRegistryFromConfig (feature/ai) fills web_search + MCP.
class ToolRegistry : public Module {
public:
  ToolRegistry();

  void Register(ToolDescriptor tool);
  void RegisterProvider(IToolProvider& provider);
  void Clear();

  const std::vector<ToolDescriptor>& Tools() const { return tools_; }
  std::vector<ToolDefinition> Definitions() const;
  std::string SummaryForPrompt() const;

  Roe<std::string> Execute(const std::string& name, const Object& arguments) const;

private:
  std::vector<ToolDescriptor> tools_;
};

} // namespace pbr
