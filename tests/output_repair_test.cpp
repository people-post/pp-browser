#include "base/ai/PromptBuilder.h"
#include "base/ai/TurnPlan.h"

#include <cassert>
#include <string>

int main() {
  pbr::TurnPlan plan;
  plan.user_request = "Why is the Fed raising rates?";
  plan.response_goal = pbr::ResponseGoal::AnswerQuestion;
  plan.synthesis_hints = "Answer directly.";

  const std::string synthesis = pbr::PromptBuilder::BuildSynthesisPrompt(plan);
  assert(synthesis.find("answer_question") != std::string::npos);
  assert(synthesis.find("Why is the Fed raising rates?") != std::string::npos);
  assert(synthesis.find("Answer directly.") != std::string::npos);

  const std::string repair =
      pbr::PromptBuilder::BuildOutputRepairPrompt(plan, "bad json", "missing blocks array");
  assert(repair.find("missing blocks array") != std::string::npos);
  assert(repair.find("bad json") != std::string::npos);

  const std::string planner = pbr::PromptBuilder::BuildPlannerPrompt();
  assert(planner.find("response_goal") != std::string::npos);
  assert(planner.find("people_list") != std::string::npos);

  const std::string system_prompt = pbr::PromptBuilder::BuildChatAgentSystemPrompt("- web_search: test\n");
  assert(system_prompt.find("REFINEMENT TOOL USE") != std::string::npos);
  assert(system_prompt.find("MUST call web_search") == std::string::npos);

  return 0;
}
