#include "agent/WorkingSetPolicy.h"

#include <nlohmann/json.hpp>

#include <cassert>
#include <iostream>

int main() {
  const auto feed_routing = pbr::RouteTurn(pbr::ResponseGoal::DisplayFeed, pbr::RenderMode::Blocks);
  assert(feed_routing.panel_primary);
  assert(feed_routing.auto_open_eligible);

  const auto answer_routing = pbr::RouteTurn(pbr::ResponseGoal::AnswerQuestion, pbr::RenderMode::Blocks);
  assert(!answer_routing.panel_primary);
  assert(answer_routing.auto_open_eligible);

  const std::string long_list = R"({
    "type": "long_list",
    "items": [{ "title": "Alice" }]
  })";
  const auto long_list_doc = nlohmann::json::parse(long_list);
  const pbr::BlockEligibility long_list_eligibility =
      pbr::EvaluateBlock(long_list_doc, pbr::ResponseGoal::PeopleDiscovery);
  assert(long_list_eligibility.eligible);
  assert(long_list_eligibility.kind == pbr::WorkingSetKind::LongList);
  assert(long_list_eligibility.affinity == pbr::WorkingSetAffinity::Feed);

  const std::string small_table = R"({
    "type": "table",
    "headers": ["A", "B"],
    "rows": [["1", "2"], ["3", "4"]]
  })";
  const auto small_table_doc = nlohmann::json::parse(small_table);
  const pbr::BlockEligibility small_table_eligibility =
      pbr::EvaluateBlock(small_table_doc, pbr::ResponseGoal::General);
  assert(!small_table_eligibility.eligible);

  const std::string large_table = R"({
    "type": "table",
    "headers": ["A", "B", "C", "D"],
    "rows": [["1","2","3","4"],["5","6","7","8"],["9","10","11","12"],["13","14","15","16"],["17","18","19","20"]]
  })";
  const auto large_table_doc = nlohmann::json::parse(large_table);
  const pbr::BlockEligibility large_table_eligibility =
      pbr::EvaluateBlock(large_table_doc, pbr::ResponseGoal::General);
  assert(large_table_eligibility.eligible);
  assert(large_table_eligibility.kind == pbr::WorkingSetKind::Table);

  const std::string feed_json = R"({"blocks":[{"type":"long_list","items":[{"title":"A"}]}]})";
  assert(pbr::InferResponseGoalFromBlocksJson(feed_json) == pbr::ResponseGoal::PeopleDiscovery);

  std::cout << "working_set_policy_test ok\n";
  return 0;
}
