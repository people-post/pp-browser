#include "feature/ai/PayloadTurnPlanBuilder.h"

#include <gtest/gtest.h>

#include <string>

TEST(PayloadTurnPlanBuilderTest, BuildsArticleSummaryPlan) {
  const std::string payload = R"({"type":"article","id":"art-001"})";
  const auto plan = pbr::TryBuildPlanFromPayload("Summarize this article", payload);
  ASSERT_TRUE(static_cast<bool>(plan));
  EXPECT_EQ(plan->source, pbr::TurnPlanSource::Payload);
  EXPECT_EQ(plan->response_goal, pbr::ResponseGoal::Summarize);
  EXPECT_TRUE(plan->tools.empty());
}

TEST(PayloadTurnPlanBuilderTest, BuildsFeedContinuationToolPlan) {
  const std::string payload = R"({"tool":"blog_articles","before_id":"art-001","size":10})";
  const auto plan = pbr::TryBuildPlanFromPayload("Load more articles", payload);
  ASSERT_TRUE(static_cast<bool>(plan));
  EXPECT_EQ(plan->response_goal, pbr::ResponseGoal::DisplayFeed);
  ASSERT_EQ(plan->tools.size(), 1u);
  EXPECT_EQ(plan->tools[0].name, "blog_articles");
  EXPECT_EQ(plan->tools[0].arguments.getString("before_id"), std::optional<std::string>("art-001"));
}

TEST(PayloadTurnPlanBuilderTest, HandlesFormSubmissionAndEmptyPayload) {
  const std::string payload =
      R"({"type":"form_submission","form_id":"booking","values":{"name":"Alice","date":"2026-06-15"}})";
  const auto form_plan = pbr::TryBuildPlanFromPayload("Booked for Alice", payload);
  ASSERT_TRUE(static_cast<bool>(form_plan));
  EXPECT_EQ(form_plan->response_goal, pbr::ResponseGoal::General);
  EXPECT_TRUE(form_plan->tools.empty());

  const auto empty_plan = pbr::TryBuildPlanFromPayload("hello", "");
  EXPECT_FALSE(static_cast<bool>(empty_plan));
}
