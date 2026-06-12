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
  assert(system_prompt.find("multiple times in one turn") != std::string::npos);
  assert(system_prompt.find("no further searches are needed") != std::string::npos);

  const std::string proactive = pbr::PromptBuilder::BuildProactiveSearchContext("ai news", json);
  assert(proactive.find("call web_search again") != std::string::npos);
  assert(proactive.find("Answer ONLY") == std::string::npos);

  return 0;
}
