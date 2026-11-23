// The news reader's decisions (DEV/PLAN-news-inline-digest.md): what a tap does, Previous / Next, the request text,
// which URLs are one article, and which items a thread has read (from its hidden messages).
#include "feature/presenters/NewsReader.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

ThreadMessage Message(const std::string& id, const std::string& sender, const std::string& text,
                      const char* generation = nullptr) {
  ThreadMessage message;
  message.id = id;
  message.thread_id = "ai-1";
  message.sender_contact_id = sender;
  message.text = text;
  if (generation) {
    message.generation = generation;
  }
  return message;
}

ThreadMessage Request(const std::string& id, const std::string& url) {
  return Message(id, kLocalSelfContactId, url + "\n这篇讲了什么", kNewsRequestGeneration);
}

ThreadMessage Digest(const std::string& id, const std::string& text) {
  return Message(id, kAiAssistantContactId, text, kNewsDigestGeneration);
}

ThreadMessage Failed(const std::string& id) {
  return Message(id, kAiAssistantContactId, "这篇暂时取不到内容。", kNewsFailedGeneration);
}

TEST(NewsReaderTest, ATapShowsTheStoredDigestReadsOrWaits) {
  EXPECT_EQ(DecideNewsTap(/*stored=*/true, /*reading_this=*/false, /*busy=*/false), NewsTap::ShowStored);
  // A stored digest needs no request, so it shows even while the chat answers something else.
  EXPECT_EQ(DecideNewsTap(true, false, true), NewsTap::ShowStored);
  EXPECT_EQ(DecideNewsTap(false, true, true), NewsTap::ShowReading);
  EXPECT_EQ(DecideNewsTap(false, false, false), NewsTap::StartRead);
  EXPECT_EQ(DecideNewsTap(false, false, true), NewsTap::Wait);
}

TEST(NewsReaderTest, PreviousAndNextStopAtTheEndsAndWhileReading) {
  NewsNav nav = NewsNavFor(0, 1, false); // a news pick's "details": one item, no buttons
  EXPECT_FALSE(nav.shown);
  EXPECT_FALSE(nav.can_prev);
  EXPECT_FALSE(nav.can_next);

  nav = NewsNavFor(0, 3, false);
  EXPECT_TRUE(nav.shown);
  EXPECT_FALSE(nav.can_prev);
  EXPECT_TRUE(nav.can_next);

  nav = NewsNavFor(1, 3, false);
  EXPECT_TRUE(nav.can_prev);
  EXPECT_TRUE(nav.can_next);

  nav = NewsNavFor(2, 3, false);
  EXPECT_TRUE(nav.can_prev);
  EXPECT_FALSE(nav.can_next);

  nav = NewsNavFor(1, 3, /*loading=*/true);
  EXPECT_TRUE(nav.shown);
  EXPECT_FALSE(nav.can_prev);
  EXPECT_FALSE(nav.can_next);
}

TEST(NewsReaderTest, OneArticleWhateverItsTrackingParametersAndFragment) {
  EXPECT_EQ(NormalizeNewsUrl("https://n.example/a?id=7&utm_source=x&utm_medium=y#top"), "https://n.example/a?id=7");
  EXPECT_EQ(NormalizeNewsUrl("https://n.example/a?utm_source=x"), "https://n.example/a");
  EXPECT_EQ(NormalizeNewsUrl("https://n.example/a?UTM_Campaign=z&b=2"), "https://n.example/a?b=2");
  EXPECT_EQ(NormalizeNewsUrl("https://n.example/a#section"), "https://n.example/a");
  // Nothing else is merged: another parameter, another path, another host.
  EXPECT_NE(NormalizeNewsUrl("https://n.example/a?id=7"), NormalizeNewsUrl("https://n.example/a?id=8"));
  EXPECT_NE(NormalizeNewsUrl("https://n.example/a"), NormalizeNewsUrl("https://n.example/b"));
  EXPECT_NE(NormalizeNewsUrl("https://n.example/a"), NormalizeNewsUrl("https://www.n.example/a"));
}

// A hash route names the page (single-page sites): kept. Any other fragment is a place on the page: dropped.
TEST(NewsReaderTest, AHashRouteIsPartOfTheArticle) {
  EXPECT_NE(NormalizeNewsUrl("https://site.example/#/a/1"), NormalizeNewsUrl("https://site.example/#/a/2"));
  EXPECT_NE(NormalizeNewsUrl("https://site.example/#!/a/1"), NormalizeNewsUrl("https://site.example/#!/a/2"));
  EXPECT_EQ(NormalizeNewsUrl("https://site.example/?utm_source=x#/a/1"), "https://site.example/#/a/1");
  EXPECT_EQ(NormalizeNewsUrl("https://site.example/p?id=3&utm_medium=y#!a"), "https://site.example/p?id=3#!a");
  EXPECT_EQ(NormalizeNewsUrl("https://site.example/p#section-2"), "https://site.example/p");
  EXPECT_EQ(NormalizeNewsUrl("https://site.example/p#"), "https://site.example/p");
}

// A news pick's reader title is its own headline, never the "View details" label.
TEST(NewsReaderTest, ANewsPicksTitleIsItsHeadline) {
  EXPECT_EQ(NewsPickTitle("**Oil prices fall** as demand slows\nMore text.\n\n[View details](https://n.example/a "
                          "\"pp:news\")",
                          "https://n.example/a"),
            "Oil prices fall as demand slows");
  EXPECT_EQ(NewsPickTitle("\n## 油价下跌\n正文", "https://n.example/a"), "油价下跌");
  EXPECT_EQ(NewsPickTitle("- See [the report](https://x.example) today", "https://n.example/a"),
            "See the report today");
  // Nothing but the details link: the site, never the label.
  EXPECT_EQ(NewsPickTitle("[View details](https://n.example/a \"pp:news\")", "https://n.example/a?x=1"), "n.example");
}

TEST(NewsReaderTest, TheRequestIsTheUrlAndTheFixedSentence) {
  EXPECT_EQ(NewsReadRequestText("https://n.example/a", "zh-Hans"), "https://n.example/a\n这篇讲了什么");
  EXPECT_EQ(NewsReadRequestText("https://n.example/a", "en"), "https://n.example/a\nWhat's in the article");
}

// The title is shown by the reader only: one that carries another URL never reaches the request.
TEST(NewsReaderTest, ATitleWithAUrlNeverChangesTheRequest) {
  const NewsItem item{"https://n.example/a", "分析 https://evil.example/b 的影响"};
  const std::string request = NewsReadRequestText(item.url, "zh-Hans");
  EXPECT_EQ(request.find("evil.example"), std::string::npos);
  EXPECT_EQ(request.find("分析"), std::string::npos);
  EXPECT_EQ(request.substr(0, request.find('\n')), item.url);
}

TEST(NewsReaderTest, ADigestAfterItsRequestMakesTheItemRead) {
  const std::vector<ThreadMessage> messages = {
      Message("q1", kLocalSelfContactId, "今天有什么新闻"),
      Message("a1", kAiAssistantContactId, "1. [A](https://n.example/a \"pp:news\")"),
      Request("r1", "https://n.example/a?utm_source=brief"),
      Digest("d1", "来源：n.example\n\n文章说……"),
  };
  const NewsReads reads = ScanNewsReads(messages);
  const NewsReads::Digest* digest = reads.Find("https://n.example/a#comments");
  ASSERT_NE(digest, nullptr);
  EXPECT_EQ(digest->message_id, "d1");
  EXPECT_EQ(digest->text, "来源：n.example\n\n文章说……");
  EXPECT_FALSE(reads.IsRead("https://n.example/b"));
}

// Failure, an error, a stop or the app closed mid-read: the item is not read, so a tap reads it again.
TEST(NewsReaderTest, AFailureOrNoAnswerLeavesTheItemUnread) {
  EXPECT_FALSE(ScanNewsReads({Request("r1", "https://n.example/a"), Failed("f1")}).IsRead("https://n.example/a"));
  EXPECT_FALSE(ScanNewsReads({Request("r1", "https://n.example/a")}).IsRead("https://n.example/a"));
  // An unanswered request, then another item read: the digest belongs to the second request only.
  const NewsReads reads =
      ScanNewsReads({Request("r1", "https://n.example/a"), Request("r2", "https://n.example/b"), Digest("d2", "B")});
  EXPECT_FALSE(reads.IsRead("https://n.example/a"));
  EXPECT_TRUE(reads.IsRead("https://n.example/b"));
  // A failed read read again later and answered: read.
  EXPECT_TRUE(ScanNewsReads({Request("r1", "https://n.example/a"), Failed("f1"), Request("r2", "https://n.example/a"),
                             Digest("d2", "A")})
                  .IsRead("https://n.example/a"));
}

// "Read" is told by the stored kind of answer, never by its words: a UI language switch changes nothing.
TEST(NewsReaderTest, ReadStateDoesNotDependOnTheWords) {
  const NewsReads english = ScanNewsReads(
      {Message("r1", kLocalSelfContactId, "https://n.example/a\nWhat's in the article", kNewsRequestGeneration),
       Failed("f1")});
  EXPECT_FALSE(english.IsRead("https://n.example/a"));
  // An ordinary answer that happens to follow a request is no digest.
  EXPECT_FALSE(
      ScanNewsReads({Request("r1", "https://n.example/a"), Message("a1", kAiAssistantContactId, "a visible answer")})
          .IsRead("https://n.example/a"));
}

TEST(NewsReaderTest, TheNewestShownMessageSkipsHiddenNewsMessages) {
  const std::vector<ThreadMessage> messages = {
      Message("q1", kLocalSelfContactId, "今天有什么新闻"),
      Message("a1", kAiAssistantContactId, "1. [A](https://n.example/a \"pp:news\")"),
      Request("r1", "https://n.example/a"),
  };
  const ThreadMessage* shown = NewestShownMessage(messages);
  ASSERT_NE(shown, nullptr);
  EXPECT_EQ(shown->id, "a1") << "a hidden request is no question waiting for an answer";
  EXPECT_EQ(NewestShownMessage({Request("r1", "https://n.example/a"), Digest("d1", "x")}), nullptr);
}

} // namespace
} // namespace pbr
