#include "base/ai/TurnPlan.h"
#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

TEST(TurnPlanTest, ParsesAndValidatesKnownTools) {
  const std::string json = R"({
    "response_goal": "answer_question",
    "tools": [{ "name": "web_search", "arguments": { "query": "fed rates" } }],
    "render_mode": "blocks",
    "synthesis_hints": "Answer first."
  })";

  const auto doc = pbr::TryParseObject(json);
  ASSERT_TRUE(doc.has_value());
  auto plan = pbr::ParseTurnPlanJson(*doc, pbr::TurnPlanSource::Planner);
  ASSERT_TRUE(static_cast<bool>(plan));
  EXPECT_EQ(plan->response_goal, pbr::ResponseGoal::AnswerQuestion);
  ASSERT_EQ(plan->tools.size(), 1u);
  EXPECT_EQ(plan->tools[0].name, "web_search");
  EXPECT_EQ(plan->render_mode, pbr::RenderMode::Blocks);

  const std::vector<std::string> allowed = {"web_search", "blog_articles"};
  auto validated = pbr::ValidateTurnPlan(*plan, allowed);
  EXPECT_TRUE(static_cast<bool>(validated));

  plan->tools.push_back({.name = "unknown_tool", .arguments = {}});
  auto rejected = pbr::ValidateTurnPlan(*plan, allowed);
  EXPECT_FALSE(static_cast<bool>(rejected));
}

TEST(TurnPlanTest, ParsesFencedJsonOutput) {
  const std::string json = R"({
    "response_goal": "answer_question",
    "tools": [{ "name": "web_search", "arguments": { "query": "fed rates" } }],
    "render_mode": "blocks",
    "synthesis_hints": "Answer first."
  })";

  const std::string fenced = "Here is the plan:\n```json\n" + json + "\n```";
  auto from_llm = pbr::ParseTurnPlanFromLlmOutput(fenced, pbr::TurnPlanSource::Planner);
  ASSERT_TRUE(static_cast<bool>(from_llm));
  ASSERT_EQ(from_llm->tools.size(), 1u);
}

TEST(TurnPlanTest, HandlesNullSynthesisHints) {
  const std::string null_hints = R"({
    "response_goal": "general",
    "tools": [{ "name": "list_conversations", "arguments": {} }],
    "render_mode": "blocks",
    "synthesis_hints": null
  })";

  auto doc = pbr::TryParseObject(null_hints);
  ASSERT_TRUE(doc.has_value());
  auto plan = pbr::ParseTurnPlanJson(*doc, pbr::TurnPlanSource::Planner);
  ASSERT_TRUE(static_cast<bool>(plan));
  ASSERT_EQ(plan->tools.size(), 1u);
  EXPECT_EQ(plan->tools[0].name, "list_conversations");
  EXPECT_TRUE(plan->synthesis_hints.empty());
}
