#include "domain/ai/ToolResultFormatter.h"

#include "domain/ai/PromptBuilder.h"
#include "common/chat/PeopleDiscoveryBlocks.h"

namespace pbr {

namespace {

std::string FormatBody(const std::string& tool_name, const std::string& raw_result) {
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

// A result may quote the end marker; swapping its brackets keeps the words and breaks the fence.
std::string NeutraliseFenceEnd(std::string body) {
  const std::string marker = kToolResultFenceEnd;
  std::string replacement = marker;
  replacement.front() = '(';
  replacement.back() = ')';
  for (size_t pos = body.find(marker); pos != std::string::npos; pos = body.find(marker, pos + replacement.size())) {
    body.replace(pos, marker.size(), replacement);
  }
  return body;
}

} // namespace

std::string FormatToolResultForLlm(const std::string& tool_name, const std::string& raw_result) {
  return std::string(kToolResultFenceBegin) + "\n" + NeutraliseFenceEnd(FormatBody(tool_name, raw_result)) + "\n" +
         kToolResultFenceEnd;
}

} // namespace pbr
