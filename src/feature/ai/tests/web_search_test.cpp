#include "feature/ai/tools/WebSearchTool.h"
#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include <string>

namespace {

const pbr::Array* AsResults(const pbr::Value& value) {
  return pbr::asArray(value);
}

const pbr::Object* AtObject(const pbr::Array& arr, size_t index) {
  return pbr::asObject(arr.elements[index]);
}

} // namespace

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
  const pbr::Array* results = AsResults(instant_results);
  ASSERT_NE(results, nullptr);
  ASSERT_EQ(results->elements.size(), 2u);
  const pbr::Object* first = AtObject(*results, 0);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->getString("title"), std::optional<std::string>("Example Topic"));
  EXPECT_EQ(first->getString("snippet"), std::optional<std::string>("Example abstract."));
}

TEST(WebSearchToolTest, ParsesLiteHtmlResults) {
  const std::string html = R"(
    <a rel="nofollow" href="https://example.com/a" class='result-link'>First Result</a>
    <td class='result-snippet'>First snippet text</td>
    <a rel="nofollow" href="https://example.com/b" class='result-link'>Second Result</a>
    <td class='result-snippet'>Second snippet text</td>
  )";

  auto html_results = pbr::WebSearchTool::ParseDuckDuckGoLiteHtmlResults(html);
  const pbr::Array* results = AsResults(html_results);
  ASSERT_NE(results, nullptr);
  ASSERT_EQ(results->elements.size(), 2u);
  const pbr::Object* first = AtObject(*results, 0);
  const pbr::Object* second = AtObject(*results, 1);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(first->getString("title"), std::optional<std::string>("First Result"));
  EXPECT_EQ(first->getString("snippet"), std::optional<std::string>("First snippet text"));
  EXPECT_EQ(second->getString("url"), std::optional<std::string>("https://example.com/b"));
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
  const pbr::Array* results = AsResults(rss_results);
  ASSERT_NE(results, nullptr);
  ASSERT_EQ(results->elements.size(), 2u);
  const pbr::Object* first = AtObject(*results, 0);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->getString("title"), std::optional<std::string>("Story One - Example News"));
  EXPECT_EQ(first->getString("snippet"), std::optional<std::string>("First story summary."));
}
