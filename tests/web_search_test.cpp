#include "agent/tools/WebSearchTool.h"

#include <stdexcept>
#include <string>

namespace {

void Expect(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

} // namespace

int main() {
  const std::string instant = R"({
    "AbstractText": "Example abstract.",
    "Heading": "Example Topic",
    "AbstractURL": "https://example.com",
    "RelatedTopics": [
      { "Text": "Related item", "FirstURL": "https://example.com/related" }
    ]
  })";

  auto instant_results = pbr::WebSearchTool::ParseDuckDuckGoInstantAnswerJson(instant);
  Expect(instant_results.size() == 2, "instant answer should yield two results");
  Expect(instant_results[0]["title"] == "Example Topic", "abstract title");
  Expect(instant_results[0]["snippet"] == "Example abstract.", "abstract snippet");

  const std::string html = R"(
    <a rel="nofollow" href="https://example.com/a" class='result-link'>First Result</a>
    <td class='result-snippet'>First snippet text</td>
    <a rel="nofollow" href="https://example.com/b" class='result-link'>Second Result</a>
    <td class='result-snippet'>Second snippet text</td>
  )";

  auto html_results = pbr::WebSearchTool::ParseDuckDuckGoLiteHtmlResults(html);
  Expect(html_results.size() == 2, "lite html should yield two results");
  Expect(html_results[0]["title"] == "First Result", "first html title");
  Expect(html_results[0]["snippet"] == "First snippet text", "first html snippet");
  Expect(html_results[1]["url"] == "https://example.com/b", "second html url");

  return 0;
}
