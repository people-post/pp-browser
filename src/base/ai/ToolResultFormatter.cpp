#include "base/ai/ToolResultFormatter.h"

#include "base/ai/PromptBuilder.h"
#include "common/PeopleDiscoveryBlocks.h"

namespace pbr {

std::string FormatToolResultForLlm(const std::string& tool_name, const std::string& raw_result) {
  if (tool_name == "web_search") {
    return PromptBuilder::FormatSearchResultsForLlm(raw_result);
  }
  if (tool_name == "search_people" || tool_name == "list_contacts") {
    if (const std::string blocks = TryPeopleDiscoveryBlocksFromToolJson(raw_result); !blocks.empty()) {
      return "People discovery results (render with long_list; do not expose this JSON verbatim):\n" + blocks;
    }
  }
  if (PromptBuilder::IsMcpArticleFeedTool(tool_name)) {
    return PromptBuilder::FormatMcpArticleResultsForLlm(raw_result);
  }
  return raw_result;
}

} // namespace pbr
