#include "domain/ai/WorkingSetPolicy.h"
#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include <string>

TEST(WorkingSetPolicyTest, RoutesFeedAndAnswerTurns) {
  const auto feed_routing = pbr::RouteTurn(pbr::ResponseGoal::DisplayFeed, pbr::RenderMode::Blocks);
  EXPECT_TRUE(feed_routing.panel_primary);
  EXPECT_TRUE(feed_routing.auto_open_eligible);

  const auto answer_routing = pbr::RouteTurn(pbr::ResponseGoal::AnswerQuestion, pbr::RenderMode::Blocks);
  EXPECT_FALSE(answer_routing.panel_primary);
  EXPECT_TRUE(answer_routing.auto_open_eligible);
}

TEST(WorkingSetPolicyTest, EvaluatesBlockEligibilityByShape) {
  const auto long_list_doc = pbr::TryParseObject(R"({
    "type": "long_list",
    "items": [{ "title": "Alice" }]
  })");
  ASSERT_TRUE(long_list_doc.has_value());
  const auto long_list_eligibility = pbr::EvaluateBlock(*long_list_doc, pbr::ResponseGoal::PeopleDiscovery);
  EXPECT_TRUE(long_list_eligibility.eligible);
  EXPECT_EQ(long_list_eligibility.kind, pbr::WorkingSetKind::LongList);
  EXPECT_EQ(long_list_eligibility.affinity, pbr::WorkingSetAffinity::Feed);

  const auto small_table_doc = pbr::TryParseObject(R"({
    "type": "table",
    "headers": ["A", "B"],
    "rows": [["1", "2"], ["3", "4"]]
  })");
  ASSERT_TRUE(small_table_doc.has_value());
  const auto small_table_eligibility = pbr::EvaluateBlock(*small_table_doc, pbr::ResponseGoal::General);
  EXPECT_FALSE(small_table_eligibility.eligible);

  const auto large_table_doc = pbr::TryParseObject(R"({
    "type": "table",
    "headers": ["A", "B", "C", "D"],
    "rows": [["1","2","3","4"],["5","6","7","8"],["9","10","11","12"],["13","14","15","16"],["17","18","19","20"]]
  })");
  ASSERT_TRUE(large_table_doc.has_value());
  const auto large_table_eligibility = pbr::EvaluateBlock(*large_table_doc, pbr::ResponseGoal::General);
  EXPECT_TRUE(large_table_eligibility.eligible);
  EXPECT_EQ(large_table_eligibility.kind, pbr::WorkingSetKind::Table);
}

TEST(WorkingSetPolicyTest, InfersPeopleDiscoveryFromBlocks) {
  const std::string feed_json = R"({"blocks":[{"type":"long_list","items":[{"title":"A"}]}]})";
  EXPECT_EQ(pbr::InferResponseGoalFromBlocksJson(feed_json), pbr::ResponseGoal::PeopleDiscovery);
}
