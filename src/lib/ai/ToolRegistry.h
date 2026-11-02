#pragma once

#include "lib/ai/IToolProvider.h"
#include "lib/ai/ToolDescriptor.h"
#include "lib/ai/LlmClient.h"
#include "common/Error.h"
#include "common/Module.h"
#include "common/PbrCompat.h"

#include <chrono>
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

  /**
   * Runs the tool. App actions (the messaging and settings providers) run one at a time across every registry in
   * the process, so turns of two AI sessions never act on the app at once; a `ui_thread` tool runs on the UI
   * thread while the caller waits at most kUiToolTimeout (then it fails and is never run late).
   */
  Roe<std::string> Execute(const std::string& name, const Object& arguments) const;

  /** How long a worker waits for the UI thread to run a `ui_thread` tool. */
  static constexpr std::chrono::seconds kUiToolTimeout{5};

private:
  std::vector<ToolDescriptor> tools_;
};

} // namespace pbr
