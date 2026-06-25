#include "feature/ai/tools/WebSearchTool.h"

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

  const std::string rss = R"(<?xml version="1.0" encoding="UTF-8"?>
    <rss><channel>
      <item>
        <title>Google News</title>
        <link>https://news.google.com/</link>
      </item>
      <item>
        <title>Story One - Example News</title>
        <link>https://example.com/one</link>
        <description>First story summary.</description>
      </item>
      <item>
        <title>Story Two - Example News</title>
        <link>https://example.com/two</link>
      </item>
    </channel></rss>)";

  auto rss_results = pbr::WebSearchTool::ParseGoogleNewsRssItems(rss);
  Expect(rss_results.size() == 2, "rss should skip feed title and yield two stories");
  Expect(rss_results[0]["title"] == "Story One - Example News", "first rss title");
  Expect(rss_results[0]["snippet"] == "First story summary.", "first rss snippet");

  return 0;
}
