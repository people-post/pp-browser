#include "feature/ai/PayloadTurnPlanBuilder.h"

#include <cassert>
#include <string>

int main() {
  {
    const std::string payload = R"({"type":"article","id":"art-001"})";
    const auto plan = pbr::TryBuildPlanFromPayload("Summarize this article", payload);
    assert(plan);
    assert(plan->source == pbr::TurnPlanSource::Payload);
    assert(plan->response_goal == pbr::ResponseGoal::Summarize);
    assert(plan->tools.empty());
  }

  {
    const std::string payload = R"({"tool":"blog_articles","before_id":"art-001","size":10})";
    const auto plan = pbr::TryBuildPlanFromPayload("Load more articles", payload);
    assert(plan);
    assert(plan->response_goal == pbr::ResponseGoal::DisplayFeed);
    assert(plan->tools.size() == 1);
    assert(plan->tools[0].name == "blog_articles");
    assert(plan->tools[0].arguments["before_id"] == "art-001");
  }

  {
    const std::string payload =
        R"({"type":"form_submission","form_id":"booking","values":{"name":"Alice","date":"2026-06-15"}})";
    const auto plan = pbr::TryBuildPlanFromPayload("Booked for Alice", payload);
    assert(plan);
    assert(plan->response_goal == pbr::ResponseGoal::General);
    assert(plan->tools.empty());
  }

  {
    const auto plan = pbr::TryBuildPlanFromPayload("hello", "");
    assert(!plan);
  }

  return 0;
}
