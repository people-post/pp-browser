#include "domain/ai/ArticleFeedBlocks.h"
#include "domain/ai/LocalizedLabels.h"
#include "domain/ai/StructuredTextParser.h"

#include <gtest/gtest.h>

using namespace pbr;

namespace {

const char* kTwoArticles =
    R"({"articles":[)"
    R"({"id":"a1","title":"","content":"First brief.","link_to":"https://user@apnews.com:8443/x?y","created_at":1759000000},)"
    R"({"id":"a2","title":"Second","content":"Body text.","link_to":"http://plain.example/y","created_at":1759000000000}]})";

} // namespace

TEST(ArticleFeedBlocksTest, BuildsIntroAndLongListThatTheParserAccepts) {
  ArticleFeedBuildOptions options;
  options.call_arguments.set("size", static_cast<int64_t>(2));
  const std::string json = BuildArticleFeedBlocksJson(kTwoArticles, options);
  ASSERT_FALSE(json.empty());
  EXPECT_NE(json.find("Latest from Brief (2):"), std::string::npos);
  EXPECT_NE(json.find("Latest briefs"), std::string::npos);

  auto parsed = StructuredTextParser::ParseBlocksJson(json, ResponseGoal::DisplayFeed);
  ASSERT_TRUE(parsed.ok) << parsed.error;
  ASSERT_EQ(parsed.working_set_candidates.size(), 1u); // the list opens in the side panel, like people results
  const std::string& rml = parsed.working_set_candidates[0].artifact_rml;
  EXPECT_NE(rml.find("First brief."), std::string::npos); // content stands in for the empty title
  EXPECT_NE(rml.find("Second"), std::string::npos);
  EXPECT_NE(rml.find("Body text."), std::string::npos); // title + content: content is the subtitle
  // The meta line shows the host only, no userinfo or port (the share-url attribute keeps the real link).
  const std::string meta_open = "chat-long-list-meta\">";
  size_t metas = 0;
  for (size_t pos = rml.find(meta_open); pos != std::string::npos; pos = rml.find(meta_open, pos + 1)) {
    const std::string meta = rml.substr(pos + meta_open.size(), rml.find("</p>", pos) - pos - meta_open.size());
    EXPECT_EQ(meta.find("8443"), std::string::npos) << meta;
    EXPECT_EQ(meta.find('@'), std::string::npos) << meta;
    ++metas;
  }
  EXPECT_GE(metas, 2u);
  EXPECT_NE(rml.find(">apnews.com"), std::string::npos);
  EXPECT_NE(rml.find("plain.example"), std::string::npos); // shown, but plain http gets no Open button

  // Each article carries what the item menu copies / shares / asks about; the link only when it is https.
  EXPECT_NE(rml.find("share-text=\"First brief.\""), std::string::npos);
  EXPECT_NE(rml.find("share-url=\"https://"), std::string::npos);
  EXPECT_EQ(rml.find("share-url=\"http://"), std::string::npos);

  // "View details" is a text link at the end of the article's own text, not a button row under it.
  const size_t link = rml.find("chat-inline-link");
  ASSERT_NE(link, std::string::npos);
  EXPECT_EQ(rml.find("chat-long-list-actions"), std::string::npos);
  const size_t text = rml.rfind("First brief.", link);
  ASSERT_NE(text, std::string::npos);
  EXPECT_EQ(rml.find("</p>", text) > link, true); // inside the same paragraph as the text
}

TEST(ArticleFeedBlocksTest, OnlyHttpsLinksGetAnOpenAction) {
  const std::string json = BuildArticleFeedBlocksJson(kTwoArticles);
  EXPECT_NE(json.find(R"("url":"https://user@apnews.com:8443/x?y")"), std::string::npos);
  EXPECT_EQ(json.find("http://plain.example"), std::string::npos);
  size_t opens = 0;
  for (size_t pos = json.find("open_url"); pos != std::string::npos; pos = json.find("open_url", pos + 1)) {
    ++opens;
  }
  EXPECT_EQ(opens, 1u);
}

TEST(ArticleFeedBlocksTest, FullPageOffersLoadMoreWithTheSameArgumentsAndBeforeId) {
  ArticleFeedBuildOptions options;
  options.call_arguments.set("brf_domain", "cn");
  options.call_arguments.set("size", static_cast<int64_t>(2));
  const std::string full = BuildArticleFeedBlocksJson(kTwoArticles, options);
  EXPECT_NE(full.find(R"("before_id":"a2")"), std::string::npos);
  EXPECT_NE(full.find(R"("brf_domain":"cn")"), std::string::npos);
  EXPECT_NE(full.find(R"("tool":"blog_articles")"), std::string::npos);

  options.call_arguments.set("size", static_cast<int64_t>(10));
  EXPECT_EQ(BuildArticleFeedBlocksJson(kTwoArticles, options).find("before_id"), std::string::npos);
}

TEST(ArticleFeedBlocksTest, EmptyFeedSaysSoAndNonFeedJsonYieldsNothing) {
  const std::string empty = BuildArticleFeedBlocksJson(R"({"articles":[]})");
  EXPECT_NE(empty.find("No articles right now."), std::string::npos);
  EXPECT_EQ(empty.find("long_list"), std::string::npos);
  EXPECT_TRUE(BuildArticleFeedBlocksJson("not json").empty());
  EXPECT_TRUE(BuildArticleFeedBlocksJson(R"([1,2])").empty());
}

// The link style is for an item's own actions; in the footer it stays a button.
TEST(ArticleFeedBlocksTest, LinkStyleInFooterIsAButton) {
  const std::string json = R"({"blocks":[{"type":"long_list","items":[{"title":"One","actions":[
      {"label":"Details","message":"Details","style":"link"}]}],
      "footer_actions":[{"label":"More","message":"More","style":"link"}]}]})";
  auto parsed = StructuredTextParser::ParseBlocksJson(json, ResponseGoal::DisplayFeed);
  ASSERT_TRUE(parsed.ok) << parsed.error;
  ASSERT_EQ(parsed.working_set_candidates.size(), 1u);
  const std::string& rml = parsed.working_set_candidates[0].artifact_rml;
  const size_t footer = rml.find("chat-long-list-footer");
  ASSERT_NE(footer, std::string::npos);
  EXPECT_NE(rml.find("chat-inline-link"), std::string::npos);          // the item's action
  EXPECT_EQ(rml.find("chat-inline-link", footer), std::string::npos);  // not the footer's
  EXPECT_NE(rml.find("chat-suggestion", footer), std::string::npos);
}

TEST(ArticleFeedBlocksTest, LocalizedLabelsFallBackToEnglishWithoutACatalog) {
  const ArticleFeedLabels labels = LocalizedArticleFeedLabels();
  EXPECT_EQ(labels.intro, ArticleFeedLabels{}.intro);
  EXPECT_EQ(labels.open, "[View details]");
}

// What McpToolAdapter really returns: the MCP result object with the JSON as text content.
TEST(ArticleFeedBlocksTest, ReadsArticlesFromTheMcpResultWrapper) {
  const std::string inner = R"({"articles":[{"id":"1","title":"","content":"A brief.","link_to":"https://example.com/a","created_at":1790000000}]})";
  pbr::Object part;
  part.set("type", "text");
  part.set("text", inner);
  pbr::Object wrapped;
  wrapped.set("content", pbr::ArrayValue({pbr::ObjectValue(std::move(part))}));
  const std::string raw = pbr::DumpJson(wrapped);

  EXPECT_EQ(pbr::UnwrapMcpTextResult(raw), inner);
  EXPECT_EQ(pbr::UnwrapMcpTextResult(inner), inner);
  EXPECT_EQ(pbr::UnwrapMcpTextResult("not json"), "not json");

  const std::string blocks = pbr::BuildArticleFeedBlocksJson(raw, {});
  EXPECT_NE(blocks.find("A brief."), std::string::npos);
  EXPECT_NE(blocks.find("example.com"), std::string::npos);
}
