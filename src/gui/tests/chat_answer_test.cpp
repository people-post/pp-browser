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
  // Browsers treat a backslash like a slash in http(s) URLs: the host ends there (PB-072).
  EXPECT_EQ(UrlHost("https://evil.com\\@good.com/x"), "evil.com");
  EXPECT_EQ(UrlHost("https://evil.com\\good.com"), "evil.com");
}

// PB-072: only a clean https URL may be handed to the OS; control characters (a newline, a NUL, an
// escape) have no place in a link and could smuggle a second URL or terminal noise into the handoff.
TEST(ChatAnswerTest, OnlyCleanHttpsUrlsAreOpenable) {
  EXPECT_TRUE(IsOpenableUrl("https://example.com/a?b=c#d"));
  EXPECT_FALSE(IsOpenableUrl("http://example.com/"));
  EXPECT_FALSE(IsOpenableUrl("https://example.com/a\nhttps://evil.com"));
  EXPECT_FALSE(IsOpenableUrl(std::string("https://example.com/a\0b", 22)));
  EXPECT_FALSE(IsOpenableUrl("https://example.com/\x1b[31m"));
  EXPECT_FALSE(IsOpenableUrl("https://example.com/\x7f"));
}

TEST(ChatAnswerTest, DetailsLinkPointsAtFirstHttpsSource) {
  const std::vector<BriefAiSource> sources = {
      {"Plain http", "http://insecure.test/", "web"},
      {"Example", "https://example.com/a", "web"},
      {"Other", "https://other.org/x", "web"},
  };
  const std::string text = WithDetailsLink("News.", sources, "View details");
  EXPECT_EQ(text, "News.\n\n[View details](https://example.com/a)");

  const ChatAnswerRml out = BuildMarkdownAnswer(text);
  ASSERT_EQ(out.links.size(), 1u);
  EXPECT_EQ(out.links[0], "https://example.com/a");
  EXPECT_NE(out.rml.find("open_chat_link('__ENTRY__', 0)"), std::string::npos);
  EXPECT_NE(out.rml.find("View details"), std::string::npos);
  EXPECT_EQ(out.rml.find("Example"), std::string::npos); // no source title, no separate list
}

TEST(ChatAnswerTest, DetailsLinkSurvivesARestart) {
  const std::string stored = WithDetailsLink("News.", {{"T", "https://example.com/a", "web"}}, "View details");
  EXPECT_EQ(ResolveChatLink(RecoverChatLinks(stored), 0), "https://example.com/a");
}

TEST(ChatAnswerTest, DetailsLinkOmittedWithoutHttpsSourceOrWhenAlreadyLinked) {
  EXPECT_EQ(WithDetailsLink("hi", {}, "View details"), "hi");
  EXPECT_EQ(WithDetailsLink("hi", {{"T", "http://a.test/", ""}}, "View details"), "hi");
  const std::string linked = "See [docs](https://docs.test/).";
  EXPECT_EQ(WithDetailsLink(linked, {{"T", "https://docs.test/", ""}}, "View details"), linked);
}

TEST(ChatAnswerTest, DetailsLinkDestinationCannotBreakOutOfTheLink) {
  const std::string text = WithDetailsLink("hi", {{"T", "https://a.test/x_(y) z<b>", ""}}, "View details");
  const ChatAnswerRml out = BuildMarkdownAnswer(text);
  ASSERT_EQ(out.links.size(), 1u);
  EXPECT_EQ(out.links[0], "https://a.test/x_%28y%29%20z%3Cb%3E");
  EXPECT_EQ(out.rml.find("<b>"), std::string::npos);
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
  EXPECT_FALSE(ResolveChatLink(links, 2));
}
