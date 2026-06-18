#include "agent/TurnPlan.h"

#include <nlohmann/json.hpp>

#include <cassert>
#include <string>
#include <vector>

int main() {
  const std::string json = R"({
    "response_goal": "answer_question",
    "tools": [{ "name": "web_search", "arguments": { "query": "fed rates" } }],
    "render_mode": "blocks",
    "synthesis_hints": "Answer first."
  })";

  const nlohmann::json doc = nlohmann::json::parse(json);
  auto plan = pbr::ParseTurnPlanJson(doc, pbr::TurnPlanSource::Planner);
  assert(plan);
  assert(plan->response_goal == pbr::ResponseGoal::AnswerQuestion);
  assert(plan->tools.size() == 1);
  assert(plan->tools[0].name == "web_search");
  assert(plan->render_mode == pbr::RenderMode::Blocks);

  const std::vector<std::string> allowed = {"web_search", "blog_articles"};
  auto validated = pbr::ValidateTurnPlan(*plan, allowed);
  assert(validated);

  plan->tools.push_back({.name = "unknown_tool", .arguments = nlohmann::json::object()});
  auto rejected = pbr::ValidateTurnPlan(*plan, allowed);
  assert(!rejected);

  const std::string fenced = "Here is the plan:\n```json\n" + json + "\n```";
  auto from_llm = pbr::ParseTurnPlanFromLlmOutput(fenced, pbr::TurnPlanSource::Planner);
  assert(from_llm);
  assert(from_llm->tools.size() == 1);

  return 0;
}
