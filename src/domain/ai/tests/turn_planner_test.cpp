#include "domain/ai/TurnPlan.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string ReadFixture(const char* name) {
  std::ifstream in(std::string(PP_BROWSER_TURN_PLAN_FIXTURES_DIR) + "/" + name);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

} // namespace

TEST(TurnPlannerTest, ParsesHeadlinesFixture) {
  const std::string fixture = ReadFixture("headlines.json");
  auto plan = pbr::ParseTurnPlanFromLlmOutput(fixture, pbr::TurnPlanSource::Planner);
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->response_goal, pbr::ResponseGoal::Headlines);
  ASSERT_FALSE(plan->tools.empty());
  EXPECT_EQ(plan->tools[0].name, "web_search");
}

TEST(TurnPlannerTest, ParsesPeopleDiscoveryFixture) {
  const std::string fixture = ReadFixture("people_discovery.json");
  auto plan = pbr::ParseTurnPlanFromLlmOutput(fixture, pbr::TurnPlanSource::Planner);
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->render_mode, pbr::RenderMode::PeopleList);
  ASSERT_FALSE(plan->tools.empty());
  EXPECT_EQ(plan->tools[0].name, "search_people");
}

TEST(TurnPlannerTest, ParsesChitchatWithNoTools) {
  const std::string fixture = ReadFixture("chitchat_no_tools.json");
  auto plan = pbr::ParseTurnPlanFromLlmOutput(fixture, pbr::TurnPlanSource::Planner);
  ASSERT_TRUE(plan);
  EXPECT_TRUE(plan->tools.empty());
  EXPECT_EQ(plan->response_goal, pbr::ResponseGoal::General);
}
