#include "base/ai/PromptBuilder.h"
#include "base/ai/TurnPlan.h"

#include <gtest/gtest.h>

#include <string>

TEST(OutputRepairTest, BuildsSynthesisAndRepairPrompts) {
  pbr::TurnPlan plan;
  plan.user_request = "Why is the Fed raising rates?";
  plan.response_goal = pbr::ResponseGoal::AnswerQuestion;
  plan.synthesis_hints = "Answer directly.";

  const std::string synthesis = pbr::PromptBuilder::BuildSynthesisPrompt(plan);
  EXPECT_NE(synthesis.find("answer_question"), std::string::npos);
  EXPECT_NE(synthesis.find("Why is the Fed raising rates?"), std::string::npos);
  EXPECT_NE(synthesis.find("Answer directly."), std::string::npos);

  const std::string repair =
      pbr::PromptBuilder::BuildOutputRepairPrompt(plan, "bad json", "missing blocks array");
  EXPECT_NE(repair.find("missing blocks array"), std::string::npos);
  EXPECT_NE(repair.find("bad json"), std::string::npos);
}

TEST(OutputRepairTest, PlannerPromptAndSystemPromptAreConstrained) {
  const std::string planner = pbr::PromptBuilder::BuildPlannerPrompt();
  EXPECT_NE(planner.find("response_goal"), std::string::npos);
  EXPECT_NE(planner.find("people_list"), std::string::npos);

  const std::string system_prompt = pbr::PromptBuilder::BuildChatAgentSystemPrompt("- web_search: test\n");
  EXPECT_NE(system_prompt.find("REFINEMENT TOOL USE"), std::string::npos);
  EXPECT_EQ(system_prompt.find("MUST call web_search"), std::string::npos);
}
