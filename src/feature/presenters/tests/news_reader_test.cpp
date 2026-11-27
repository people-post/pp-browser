// The news reader's decisions (DEV/PLAN-news-inline-digest.md): what a tap does, Previous / Next, the request text,
// which URLs are one article, and which items a thread has read (from its hidden messages).
#include "feature/presenters/NewsReader.h"

#include <gtest/gtest.h>

#include <chrono>
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

ThreadMessage TranslatedDigest(const std::string& id, const std::string& text) {
  return Message(id, kAiAssistantContactId, text, kNewsDigestTranslatedGeneration);
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

  nav = NewsNavFor(1, 3, /*reading=*/true);
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

// A translated digest counts as read like any digest, and says it is a translation.
TEST(NewsReaderTest, ATranslatedDigestIsReadAndKnownAsTranslated) {
  const NewsReads reads = ScanNewsReads({Request("r1", "https://n.example/a"), TranslatedDigest("d1", "译文"),
                                         Request("r2", "https://n.example/b"), Digest("d2", "same language")});
  ASSERT_NE(reads.Find("https://n.example/a"), nullptr);
  EXPECT_TRUE(reads.Find("https://n.example/a")->translated);
  ASSERT_NE(reads.Find("https://n.example/b"), nullptr);
  EXPECT_FALSE(reads.Find("https://n.example/b")->translated);
}

// The "AI translation of the original" line: only on a translated digest; not on another digest, while loading, or on
// the failure line.
TEST(NewsReaderTest, TheTranslationLineShowsOnlyOnATranslatedDigest) {
  const NewsReads reads = ScanNewsReads({Request("r1", "https://n.example/t"), TranslatedDigest("d1", "译文"),
                                         Request("r2", "https://n.example/u"), Digest("d2", "same language")});
  NewsReaderShows shows = NewsReaderShowsFor(reads, "https://n.example/t", "");
  ASSERT_NE(shows.digest, nullptr);
  EXPECT_TRUE(shows.translated);
  EXPECT_FALSE(shows.loading || shows.failed);

  shows = NewsReaderShowsFor(reads, "https://n.example/u", "");
  ASSERT_NE(shows.digest, nullptr);
  EXPECT_FALSE(shows.translated);

  shows = NewsReaderShowsFor(reads, "https://n.example/new", "https://n.example/new");
  EXPECT_TRUE(shows.loading);
  EXPECT_FALSE(shows.translated);
  EXPECT_EQ(shows.digest, nullptr);

  shows = NewsReaderShowsFor(reads, "https://n.example/new", "");
  EXPECT_TRUE(shows.failed);
  EXPECT_FALSE(shows.translated);
}

// A read that streams: the spinner until the first text, then the text so far (Previous / Next off throughout); when it
// ends with a digest the stored text (done's response) shows, whatever had streamed.
TEST(NewsReaderTest, AStreamingReadShowsItsTextSoFarThenTheStoredDigest) {
  const std::string url = "https://n.example/a";
  NewsReaderShows shows = NewsReaderShowsFor({}, url, url, "");
  EXPECT_TRUE(shows.reading);
  EXPECT_TRUE(shows.loading) << "nothing yet: the spinner line";
  EXPECT_TRUE(shows.partial.empty());
  EXPECT_FALSE(NewsNavFor(1, 3, shows.reading).can_prev);

  const std::string streamed = "n.example \u00b7 2026-11-24\n\nThe article says";
  shows = NewsReaderShowsFor({}, url + "?utm_source=brief", url, streamed);
  EXPECT_TRUE(shows.reading);
  EXPECT_FALSE(shows.loading) << "the first token hides the spinner";
  EXPECT_EQ(shows.partial, streamed);
  EXPECT_FALSE(shows.failed);
  EXPECT_EQ(shows.digest, nullptr);
  const NewsNav nav = NewsNavFor(1, 3, shows.reading);
  EXPECT_TRUE(nav.shown);
  EXPECT_FALSE(nav.can_prev || nav.can_next) << "Previous / Next stay off until the read ends";

  // Another item of the list while this one reads: not its text.
  shows = NewsReaderShowsFor({}, "https://n.example/b", url, streamed);
  EXPECT_FALSE(shows.reading);
  EXPECT_TRUE(shows.partial.empty());

  // Ended with a digest (the sheet open, or closed meanwhile and opened again): done's text, the translation line per
  // done.translated, the buttons back.
  const std::string final_text = "n.example \u00b7 2026-11-24\n\nThe article says rates rose.";
  for (const bool translated : {false, true}) {
    const NewsReads reads =
        ScanNewsReads({Request("r1", url), translated ? TranslatedDigest("d1", final_text) : Digest("d1", final_text)});
    shows = NewsReaderShowsFor(reads, url, "");
    ASSERT_NE(shows.digest, nullptr);
    EXPECT_EQ(shows.digest->text, final_text);
    EXPECT_EQ(shows.translated, translated);
    EXPECT_FALSE(shows.reading || shows.loading || shows.failed);
    EXPECT_TRUE(shows.partial.empty());
    EXPECT_TRUE(NewsNavFor(1, 3, shows.reading).can_next);
  }
}

// Text streamed, then done said "news_unavailable" (or the read was stopped or failed): the streamed text is gone,
// the fixed sentence shows, and the item is not read.
TEST(NewsReaderTest, UnavailableAfterStreamedTextShowsTheFailureAndLeavesTheItemUnread) {
  const std::string url = "https://n.example/a";
  for (const bool answered : {true, false}) { // news_unavailable stored; or nothing stored (stop, error)
    const NewsReads reads =
        answered ? ScanNewsReads({Request("r1", url), Failed("f1")}) : ScanNewsReads({Request("r1", url)});
    const NewsReaderShows shows = NewsReaderShowsFor(reads, url, /*reading_url=*/"", /*streamed=*/"");
    EXPECT_TRUE(shows.failed);
    EXPECT_FALSE(shows.reading || shows.loading);
    EXPECT_TRUE(shows.partial.empty());
    EXPECT_FALSE(reads.IsRead(url));
  }
}

// The reader redraws the growing text at most every 100 ms; a skipped redraw is drawn by the next due one, with the
// newest text, so throttling never loses the end of what streamed.
TEST(NewsReaderTest, TheStreamRedrawIsThrottledWithoutDroppingText) {
  using namespace std::chrono_literals;
  const auto t0 = std::chrono::steady_clock::time_point{} + 10s;
  NewsStreamText stream;
  EXPECT_FALSE(stream.DrawDue(t0)) << "nothing streamed yet";
  stream.Grow("src");
  EXPECT_TRUE(stream.DrawDue(t0)) << "the first text draws at once";
  stream.Drawn(t0);
  std::string text = "src";
  for (int i = 1; i <= 9; ++i) { // nine more tokens within 90 ms: no redraw
    text += " word";
    stream.Grow(text);
    EXPECT_FALSE(stream.DrawDue(t0 + std::chrono::milliseconds(10 * i))) << i;
  }
  EXPECT_TRUE(stream.DrawDue(t0 + 100ms)) << "the tick after the interval draws";
  EXPECT_EQ(stream.text, text) << "with all of it";
  stream.Drawn(t0 + 100ms);
  stream.Grow(text);
  EXPECT_FALSE(stream.DrawDue(t0 + 1s)) << "the same text again needs no redraw";
}

// The failure line comes with "Try again" (every failure kind); a digest, the spinner or streamed text never does.
TEST(NewsReaderTest, OnlyTheFailureLineOffersTryAgain) {
  const std::string url = "https://n.example/a";
  NewsReaderShows shows = NewsReaderShowsFor(ScanNewsReads({Request("r1", url), Failed("f1")}), url, "");
  EXPECT_TRUE(shows.failed);
  EXPECT_TRUE(shows.can_retry);
  shows = NewsReaderShowsFor(ScanNewsReads({Request("r1", url)}), url, ""); // stopped, or an error: nothing stored
  EXPECT_TRUE(shows.can_retry);
  // Tapped: the read runs again in place, with the spinner and then its text, and the buttons off.
  shows = NewsReaderShowsFor(ScanNewsReads({Request("r1", url), Failed("f1")}), url, url, "");
  EXPECT_FALSE(shows.failed || shows.can_retry);
  EXPECT_TRUE(shows.loading);
  EXPECT_FALSE(NewsNavFor(1, 3, shows.reading).can_next);
  shows = NewsReaderShowsFor(ScanNewsReads({Request("r1", url), Failed("f1")}), url, url, "n.example");
  EXPECT_FALSE(shows.failed || shows.can_retry || shows.loading);
  // It ends with a digest: read (the item greys), no button.
  const NewsReads reads = ScanNewsReads({Request("r1", url), Failed("f1"), Request("r2", url), Digest("d2", "A")});
  EXPECT_TRUE(reads.IsRead(url));
  shows = NewsReaderShowsFor(reads, url, "");
  EXPECT_FALSE(shows.failed || shows.can_retry);
  ASSERT_NE(shows.digest, nullptr);
  EXPECT_EQ(shows.digest->message_id, "d2");
  // It fails again: the line and the button again.
  shows =
      NewsReaderShowsFor(ScanNewsReads({Request("r1", url), Failed("f1"), Request("r2", url), Failed("f2")}), url, "");
  EXPECT_TRUE(shows.failed && shows.can_retry);
}

// Retries add a request (and an answer, or none) each time; the item's state is its latest read's outcome.
TEST(NewsReaderTest, ReadStateIsTheLatestOutcomeAcrossRetries) {
  const std::string url = "https://n.example/a";
  // Failed, stopped (unanswered), failed, then a digest: read, with that digest.
  NewsReads reads = ScanNewsReads({Request("r1", url), Failed("f1"), Request("r2", url), Request("r3", url),
                                   Failed("f3"), Request("r4", url), Digest("d4", "final")});
  ASSERT_TRUE(reads.IsRead(url));
  EXPECT_EQ(reads.Find(url)->message_id, "d4");
  EXPECT_EQ(reads.Find(url)->text, "final");
  // The newest attempt still unanswered after failures: not read.
  reads = ScanNewsReads({Request("r1", url), Failed("f1"), Request("r2", url)});
  EXPECT_FALSE(reads.IsRead(url));
  // Another item's failure between this item's request and digest does not take the digest away from it.
  reads = ScanNewsReads({Request("r1", url), Failed("f1"), Request("r2", url), Digest("d2", "A"),
                         Request("r3", "https://n.example/b"), Failed("f3")});
  EXPECT_TRUE(reads.IsRead(url));
  EXPECT_FALSE(reads.IsRead("https://n.example/b"));
}

// "Try again" ignores taps for a second after the failure line appears; after that each tap counts.
TEST(NewsReaderTest, TryAgainIgnoresTapsJustAfterTheFailureShows) {
  using namespace std::chrono_literals;
  const auto shown = std::chrono::steady_clock::time_point{} + 10s;
  EXPECT_FALSE(NewsRetryTakesTap(std::nullopt, shown)) << "no failure line, nothing to retry";
  EXPECT_FALSE(NewsRetryTakesTap(shown, shown));
  EXPECT_FALSE(NewsRetryTakesTap(shown, shown + 999ms));
  EXPECT_TRUE(NewsRetryTakesTap(shown, shown + 1s));
  EXPECT_TRUE(NewsRetryTakesTap(shown, shown + 5s));
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
  EXPECT_EQ(NewestShownMessage({Request("r1", "https://n.example/a"), TranslatedDigest("d1", "x")}), nullptr);
}

} // namespace
} // namespace pbr
