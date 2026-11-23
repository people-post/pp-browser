#include "gui/chat/ChatAnswer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include "common/PbrCompat.h"

using namespace pbr;
using std::chrono::milliseconds;
using Clock = std::chrono::steady_clock;

namespace {

/** The URL a click on link `index` would act on ("" when none). */
std::string ResolvedUrl(const std::vector<MarkdownLink>& links, const int index) {
  const std::optional<MarkdownLink> link = ResolveChatLink(links, index);
  return link ? link->url : std::string();
}

/** The bubble as stored: its links' entry placeholder replaced by the message id "m-1". */
std::string InjectEntryPlaceholdersForTest(std::string rml) {
  for (size_t at = rml.find("__ENTRY__"); at != std::string::npos; at = rml.find("__ENTRY__", at)) {
    rml.replace(at, 9, "m-1");
  }
  return rml;
}

} // namespace

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
  EXPECT_EQ(out.links[0].url, "https://example.com/a");
  EXPECT_NE(out.rml.find("open_chat_link('__ENTRY__', 0)"), std::string::npos);
  EXPECT_NE(out.rml.find("View details"), std::string::npos);
  EXPECT_EQ(out.rml.find("Example"), std::string::npos); // no source title, no separate list
}

TEST(ChatAnswerTest, DetailsLinkSurvivesARestart) {
  const std::string stored = WithDetailsLink("News.", {{"T", "https://example.com/a", "web"}}, "View details");
  EXPECT_EQ(ResolvedUrl(RecoverChatLinks(stored), 0), "https://example.com/a");
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
  EXPECT_EQ(out.links[0].url, "https://a.test/x_%28y%29%20z%3Cb%3E");
  EXPECT_EQ(out.rml.find("<b>"), std::string::npos);
}

TEST(ChatAnswerTest, ResolveChatLinkOnlyOpensInRangeHttps) {
  const std::vector<MarkdownLink> links = {{"https://a.test/"}, {"javascript:alert(1)"}, {"http://b.test/"}};
  EXPECT_EQ(ResolvedUrl(links, 0), "https://a.test/");
  EXPECT_FALSE(ResolveChatLink(links, 1));
  EXPECT_FALSE(ResolveChatLink(links, 2));
  EXPECT_FALSE(ResolveChatLink(links, 3));
  EXPECT_FALSE(ResolveChatLink(links, -1));
}

TEST(ChatAnswerTest, RecoveredLinksComeFromStoredMarkdownOnly) {
  const auto links = RecoverChatLinks("a [x](https://x.test/) b [y](http://y.test/) c [z](https://z.test/)");
  EXPECT_EQ(ResolvedUrl(links, 0), "https://x.test/");
  EXPECT_EQ(ResolvedUrl(links, 1), "https://z.test/");
  EXPECT_FALSE(ResolveChatLink(links, 2));
}

// A single news pick (done.answer_kind "news_pick"): the details link is marked, so a tap opens the reader.
TEST(ChatAnswerTest, DetailsLinkIsANewsItemOnlyForANewsPick) {
  const std::vector<BriefAiSource> sources = {{"T", "https://example.com/a", "web"}};
  const std::string pick = WithDetailsLink("News.", sources, "View details", /*news_pick=*/true);
  EXPECT_EQ(pick, "News.\n\n[View details](https://example.com/a \"pp:news\")");
  const ChatAnswerRml out = BuildMarkdownAnswer(pick, /*news_links=*/true);
  ASSERT_EQ(out.links.size(), 1u);
  EXPECT_TRUE(out.links[0].news);
  EXPECT_EQ(out.links[0].label, "View details");
  // After a restart, the stored text gives the same news item back.
  const std::vector<MarkdownLink> recovered = RecoverChatLinks(pick, /*news_links=*/true);
  ASSERT_EQ(recovered.size(), 1u);
  EXPECT_TRUE(recovered[0].news);

  const std::string plain = WithDetailsLink("News.", sources, "View details");
  EXPECT_EQ(plain, "News.\n\n[View details](https://example.com/a)");
  EXPECT_FALSE(BuildMarkdownAnswer(plain, /*news_links=*/true).links[0].news);
}

TEST(ChatAnswerTest, ReadNewsItemsTurnGreyOthersStay) {
  const std::string list =
      "1. [A](https://a.test/1 \"pp:news\")\n2. [B](https://b.test/2 \"pp:news\")\n"
      "3. [C](https://c.test/3 \"pp:news\")\n\nSee [docs](https://docs.test/).";
  const ChatAnswerRml out = BuildMarkdownAnswer(list, /*news_links=*/true);
  ASSERT_EQ(out.links.size(), 4u);
  const std::string stored = InjectEntryPlaceholdersForTest(out.rml);
  const std::string marked = MarkNewsLinksRead(stored, out.links, [](const std::string& url) {
    return url == "https://b.test/2" || url == "https://docs.test/";
  });
  EXPECT_NE(marked.find("<span class=\"chat-link news-link news-read\" data-event-click=\"open_chat_link('m-1', 1)\">"),
            std::string::npos)
      << marked;
  EXPECT_NE(marked.find("<span class=\"chat-link news-link\" data-event-click=\"open_chat_link('m-1', 0)\">"),
            std::string::npos);
  EXPECT_NE(marked.find("<span class=\"chat-link news-link\" data-event-click=\"open_chat_link('m-1', 2)\">"),
            std::string::npos);
  // Only news items are ever marked: a plain link is no news item, read or not.
  EXPECT_NE(marked.find("<span class=\"chat-link\" data-event-click=\"open_chat_link('m-1', 3)\">"), std::string::npos);
  EXPECT_EQ(std::count(marked.begin(), marked.end(), '<'), std::count(stored.begin(), stored.end(), '<'));
  // Nothing read: unchanged.
  EXPECT_EQ(MarkNewsLinksRead(stored, out.links, [](const std::string&) { return false; }), stored);
}
