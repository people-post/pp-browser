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
  EXPECT_NE(rml.find("apnews.com"), std::string::npos); // host only, no userinfo or port
  EXPECT_EQ(rml.find("8443"), std::string::npos);
  EXPECT_NE(rml.find("plain.example"), std::string::npos); // shown, but plain http gets no Open button
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

TEST(ArticleFeedBlocksTest, LocalizedLabelsFallBackToEnglishWithoutACatalog) {
  const ArticleFeedLabels labels = LocalizedArticleFeedLabels();
  EXPECT_EQ(labels.intro, ArticleFeedLabels{}.intro);
  EXPECT_EQ(labels.open, "Open");
}
