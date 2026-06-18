#pragma once

#include "agent/LlmClient.h"
#include "agent/TurnPlan.h"
#include "agent/ToolRegistry.h"
#include "common/Error.h"

#include <string>
#include <vector>

namespace pbr {

class TurnPlanner {
public:
  static Roe<TurnPlan> Plan(const LlmClient& llm, const std::vector<ChatMessage>& context_messages,
                            const std::string& tools_summary, const std::vector<std::string>& allowed_tools,
                            const std::string& user_request);

private:
  static Roe<TurnPlan> PlanOnce(const LlmClient& llm, const std::vector<ChatMessage>& messages,
                                const std::vector<std::string>& allowed_tools, const std::string& user_request,
                                bool repair, const std::string& invalid_output, const std::string& error_message);
};

} // namespace pbr
