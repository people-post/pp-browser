#pragma once

#include "agent/TurnResponseIntent.h"

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
  static std::string BuildTurnResponsePolicy(const TurnResponseIntent& intent);
  static std::string BuildPostToolSynthesisReminder(const TurnResponseIntent& intent);
  static std::string BuildProactiveSearchContext(const std::string& query, const std::string& search_results,
                                                 const TurnResponseIntent& intent);
  static std::string FormatSearchResultsForLlm(const std::string& search_results_json);
  static std::string FormatMcpArticleResultsForLlm(const std::string& raw_result);
  static bool IsMcpArticleFeedTool(const std::string& tool_name);
};

} // namespace pbr
