#include "base/ai/PromptBuilder.h"

#include <gtest/gtest.h>

#include <string>

TEST(PromptBuilderTest, FormatsSearchResultsAndSystemPrompt) {
  const std::string json = R"({"results":[
    {"title":"Story One","snippet":"First summary.","url":"https://example.com/one"},
    {"title":"Story Two","snippet":"","url":"https://example.com/two"}
  ]})";

  const std::string formatted = pbr::PromptBuilder::FormatSearchResultsForLlm(json);
  EXPECT_NE(formatted.find("1. Story One"), std::string::npos);
  EXPECT_NE(formatted.find("First summary."), std::string::npos);
  EXPECT_NE(formatted.find("2. Story Two"), std::string::npos);

  const std::string system_prompt = pbr::PromptBuilder::BuildChatAgentSystemPrompt("- web_search: test\n");
  EXPECT_NE(system_prompt.find("REFINEMENT TOOL USE"), std::string::npos);
  EXPECT_NE(system_prompt.find("USER INTENT PRIORITY"), std::string::npos);
}

TEST(PromptBuilderTest, FormatsArticleFeedAndDetectsTool) {
  const std::string article_json = R"({"articles":[
    {"id":"art-001","title":"Market outlook","subtitle":"Cross-asset view","meta":"2025-06-10"}
  ]})";
  const std::string article_formatted = pbr::PromptBuilder::FormatMcpArticleResultsForLlm(article_json);
  EXPECT_NE(article_formatted.find("map to long_list"), std::string::npos);
  EXPECT_NE(article_formatted.find("Market outlook"), std::string::npos);
  EXPECT_NE(article_formatted.find("art-001"), std::string::npos);
  EXPECT_TRUE(pbr::PromptBuilder::IsMcpArticleFeedTool("blog_articles"));
}

TEST(PromptBuilderTest, PlannerPromptIncludesLiveToolCatalog) {
  const std::string summary = "- search_people [people, read]: Find people\n"
                              "- add_contact [people, write]: Add a contact\n";
  const std::string planner = pbr::PromptBuilder::BuildPlannerPrompt(summary);
  EXPECT_NE(planner.find("AVAILABLE TOOLS"), std::string::npos);
  EXPECT_NE(planner.find("search_people [people, read]"), std::string::npos);
  EXPECT_NE(planner.find("add_contact [people, write]"), std::string::npos);
  EXPECT_NE(planner.find("only plan tools listed here"), std::string::npos);
}
