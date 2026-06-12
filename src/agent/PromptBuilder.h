#pragma once

#include <string>
#include <vector>

namespace pbr {

struct McpTool;

class PromptBuilder {
public:
  static std::string DefaultRcssProfile();
  static std::string ChatBlocksProfile();

  static std::string BuildUiGenerationPrompt(const std::string& tools_context,
                                             const std::string& rml_profile);

  static std::string BuildChatSystemPrompt();
  static std::string BuildChatAgentSystemPrompt(const std::string& tools_summary);
  static std::string BuildProactiveSearchContext(const std::string& query, const std::string& search_results);
};

} // namespace pbr
