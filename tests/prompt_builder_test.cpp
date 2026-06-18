#include "agent/PromptBuilder.h"

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
  assert(system_prompt.find("REFINEMENT TOOL USE") != std::string::npos);
  assert(system_prompt.find("USER INTENT PRIORITY") != std::string::npos);

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
