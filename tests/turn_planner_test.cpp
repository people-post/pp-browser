#include "agent/TurnPlan.h"

#include <cassert>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string ReadFixture(const char* name) {
  std::ifstream in(std::string("tests/fixtures/turn_plans/") + name);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

} // namespace

int main() {
  {
    const std::string fixture = ReadFixture("headlines.json");
    auto plan = pbr::ParseTurnPlanFromLlmOutput(fixture, pbr::TurnPlanSource::Planner);
    assert(plan);
    assert(plan->response_goal == pbr::ResponseGoal::Headlines);
    assert(!plan->tools.empty());
    assert(plan->tools[0].name == "web_search");
  }

  {
    const std::string fixture = ReadFixture("people_discovery.json");
    auto plan = pbr::ParseTurnPlanFromLlmOutput(fixture, pbr::TurnPlanSource::Planner);
    assert(plan);
    assert(plan->render_mode == pbr::RenderMode::PeopleList);
    assert(plan->tools[0].name == "search_people");
  }

  {
    const std::string fixture = ReadFixture("chitchat_no_tools.json");
    auto plan = pbr::ParseTurnPlanFromLlmOutput(fixture, pbr::TurnPlanSource::Planner);
    assert(plan);
    assert(plan->tools.empty());
    assert(plan->response_goal == pbr::ResponseGoal::General);
  }

  return 0;
}
