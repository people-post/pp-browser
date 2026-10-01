#include "gui/chat/ChatAnswer.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

using namespace pbr;
using std::chrono::milliseconds;
using Clock = std::chrono::steady_clock;

TEST(ChatAnswerTest, StreamRenderThrottleHoldsFor100ms) {
  const auto t0 = Clock::now();
  EXPECT_TRUE(ShouldRenderStreamDelta(std::nullopt, t0));
  EXPECT_FALSE(ShouldRenderStreamDelta(t0, t0 + milliseconds(99)));
  EXPECT_TRUE(ShouldRenderStreamDelta(t0, t0 + milliseconds(100)));
}

TEST(ChatAnswerTest, UrlHostDropsUserinfoPortAndPath) {
  EXPECT_EQ(UrlHost("https://example.com/a?b#c"), "example.com");
  EXPECT_EQ(UrlHost("https://example.com:8443/x"), "example.com");
  EXPECT_EQ(UrlHost("https://good.com@evil.com/x"), "evil.com");
  EXPECT_EQ(UrlHost("http://example.com"), "");
}

TEST(ChatAnswerTest, SourcesExtendMarkdownLinksAndReuseExistingIndex) {
  const std::vector<BriefAiSource> sources = {
      {"Example", "https://example.com/a", "web"},
      {"", "https://other.org/x", "web"},
      {"Plain http", "http://insecure.test/", "web"},
      {"Again", "https://example.com/a", "web"},
  };
  const ChatAnswerRml out = BuildMarkdownAnswer("See [docs](https://docs.test/).", sources, "Sources");
  ASSERT_EQ(out.links.size(), 3u);
  EXPECT_EQ(out.links[0], "https://docs.test/");
  EXPECT_EQ(out.links[1], "https://example.com/a");
  EXPECT_EQ(out.links[2], "https://other.org/x");
  EXPECT_NE(out.rml.find("<div class=\"chat-sources\">"), std::string::npos);
  EXPECT_NE(out.rml.find("open_chat_link('__ENTRY__', 2)\">other.org</span>"), std::string::npos); // host fallback
  EXPECT_EQ(out.rml.find("insecure.test"), std::string::npos);
}

TEST(ChatAnswerTest, SourcesAreEscapedCappedAndOmittedWhenEmpty) {
  EXPECT_EQ(BuildMarkdownAnswer("hi", {}, "Sources").rml.find("chat-sources"), std::string::npos);

  const ChatAnswerRml escaped = BuildMarkdownAnswer("hi", {{"<b>x</b>", "https://a.test/", ""}}, "Sources");
  EXPECT_EQ(escaped.rml.find("<b>x"), std::string::npos);

  std::vector<BriefAiSource> many;
  for (int i = 0; i < 15; ++i) {
    many.push_back({"t", "https://s" + std::to_string(i) + ".test/", ""});
  }
  EXPECT_EQ(BuildMarkdownAnswer("hi", many, "Sources").links.size(), kMaxAnswerSources);
}

TEST(ChatAnswerTest, ResolveChatLinkOnlyOpensInRangeHttps) {
  const std::vector<std::string> links = {"https://a.test/", "javascript:alert(1)", "http://b.test/"};
  EXPECT_EQ(ResolveChatLink(links, 0), "https://a.test/");
  EXPECT_FALSE(ResolveChatLink(links, 1));
  EXPECT_FALSE(ResolveChatLink(links, 2));
  EXPECT_FALSE(ResolveChatLink(links, 3));
  EXPECT_FALSE(ResolveChatLink(links, -1));
}

TEST(ChatAnswerTest, RecoveredLinksComeFromStoredMarkdownOnly) {
  const auto links = RecoverChatLinks("a [x](https://x.test/) b [y](http://y.test/) c [z](https://z.test/)");
  EXPECT_EQ(ResolveChatLink(links, 0), "https://x.test/");
  EXPECT_EQ(ResolveChatLink(links, 1), "https://z.test/");
  EXPECT_FALSE(ResolveChatLink(links, 2)); // source indices are not recoverable
}
