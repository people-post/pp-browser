#pragma once

#include "domain/ai/TurnPlan.h"

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

  /** `brief_backend`: the answer comes from Brief AI (default preset); other presets get a neutral identity. */
  static std::string BuildChatSystemPrompt(bool brief_backend = true);
  static std::string BuildChatAgentSystemPrompt(const std::string& tools_summary, bool brief_backend = true);
  // `tools_summary` is the live ToolRegistry catalog (name [domain, risk]: description).
  static std::string BuildPlannerPrompt(const std::string& tools_summary = {});
  static std::string BuildPlannerRepairPrompt(const std::string& error_message);
  static std::string BuildSynthesisPrompt(const TurnPlan& plan);
  static std::string BuildSynthesisRefinementReminder(const TurnPlan& plan);
  static std::string BuildOutputRepairPrompt(const TurnPlan& plan, const std::string& raw_output,
                                             const std::string& parse_error);
  static std::string BuildScopedAssistSystemPrompt(const std::string& tools_summary, bool brief_backend = true);

  static std::string FormatSearchResultsForLlm(const std::string& search_results_json);
  static std::string FormatMcpArticleResultsForLlm(const std::string& raw_result);
  static bool IsMcpArticleFeedTool(const std::string& tool_name);
};

} // namespace pbr
