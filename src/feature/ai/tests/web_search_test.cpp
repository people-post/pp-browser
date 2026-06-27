#include "feature/ai/tools/WebSearchTool.h"

#include <gtest/gtest.h>

#include <string>

TEST(WebSearchToolTest, ParsesInstantAnswerJson) {
  const std::string instant = R"({
    "AbstractText": "Example abstract.",
    "Heading": "Example Topic",
    "AbstractURL": "https://example.com",
    "RelatedTopics": [
      { "Text": "Related item", "FirstURL": "https://example.com/related" }
    ]
  })";

  auto instant_results = pbr::WebSearchTool::ParseDuckDuckGoInstantAnswerJson(instant);
  ASSERT_EQ(instant_results.size(), 2u);
  EXPECT_EQ(instant_results[0]["title"], "Example Topic");
  EXPECT_EQ(instant_results[0]["snippet"], "Example abstract.");
}

TEST(WebSearchToolTest, ParsesLiteHtmlResults) {
  const std::string html = R"(
    <a rel="nofollow" href="https://example.com/a" class='result-link'>First Result</a>
    <td class='result-snippet'>First snippet text</td>
    <a rel="nofollow" href="https://example.com/b" class='result-link'>Second Result</a>
    <td class='result-snippet'>Second snippet text</td>
  )";

  auto html_results = pbr::WebSearchTool::ParseDuckDuckGoLiteHtmlResults(html);
  ASSERT_EQ(html_results.size(), 2u);
  EXPECT_EQ(html_results[0]["title"], "First Result");
  EXPECT_EQ(html_results[0]["snippet"], "First snippet text");
  EXPECT_EQ(html_results[1]["url"], "https://example.com/b");
}

TEST(WebSearchToolTest, ParsesGoogleNewsRssItems) {
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
  ASSERT_EQ(rss_results.size(), 2u);
  EXPECT_EQ(rss_results[0]["title"], "Story One - Example News");
  EXPECT_EQ(rss_results[0]["snippet"], "First story summary.");
}
