#include "agent/PromptBuilder.h"
#include "agent/TurnResponseIntent.h"

#include <cassert>
#include <string>

int main() {
  const std::string json = R"({"results":[
    {"title":"Story One","snippet":"First summary.","url":"https://example.com/one"},
    {"title":"Story Two","snippet":"","url":"https://example.com/two"}
  ]})";

  const std::string formatted = pbr::PromptBuilder::FormatSearchResultsForLlm(json);
  assert(formatted.find("1. Story One") != std::string::npos);
  assert(formatted.find("First summary.") != std::string::npos);
  assert(formatted.find("2. Story Two") != std::string::npos);

  const std::string system_prompt = pbr::PromptBuilder::BuildChatAgentSystemPrompt("- web_search: test\n");
  assert(system_prompt.find("multiple times in one turn") != std::string::npos);
  assert(system_prompt.find("no further searches are needed") != std::string::npos);
  assert(system_prompt.find("USER INTENT PRIORITY") != std::string::npos);

  const pbr::TurnResponseIntent headlines_intent{
      .goal = pbr::ResponseGoal::Headlines,
      .user_request = "Show me some today's headlines",
  };
  const std::string proactive = pbr::PromptBuilder::BuildProactiveSearchContext("ai news", json, headlines_intent);
  assert(proactive.find("User request:") != std::string::npos);
  assert(proactive.find("Show me some today's headlines") != std::string::npos);
  assert(proactive.find("one item per real headline") != std::string::npos);

  const pbr::TurnResponseIntent answer_intent{
      .goal = pbr::ResponseGoal::AnswerQuestion,
      .user_request = "Why is the Fed raising rates?",
  };
  const std::string answer_context = pbr::PromptBuilder::BuildProactiveSearchContext("fed rates", json, answer_intent);
  assert(answer_context.find("Why is the Fed raising rates?") != std::string::npos);
  assert(answer_context.find("Do not only list headlines") != std::string::npos);
  assert(answer_context.find("one item per real headline when possible") == std::string::npos);

  const pbr::TurnResponseIntent feed_intent{
      .goal = pbr::ResponseGoal::DisplayFeed,
      .user_request = "Show me articles from brief",
  };
  const std::string policy = pbr::PromptBuilder::BuildTurnResponsePolicy(feed_intent);
  assert(policy.find("display_feed") != std::string::npos);
  assert(policy.find("long_list") != std::string::npos);

  const std::string article_json = R"({"articles":[
    {"id":"art-001","title":"Market outlook","subtitle":"Cross-asset view","meta":"2025-06-10"}
  ]})";
  const std::string article_formatted = pbr::PromptBuilder::FormatMcpArticleResultsForLlm(article_json);
  assert(article_formatted.find("map to long_list") != std::string::npos);
  assert(article_formatted.find("Market outlook") != std::string::npos);
  assert(article_formatted.find("art-001") != std::string::npos);
  assert(pbr::PromptBuilder::IsMcpArticleFeedTool("blog_articles"));

  return 0;
}
