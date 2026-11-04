// Home's Today's briefs widget (DESIGN-pp-ui 8.16; Kenneth 2026-11-03): refetched when the last fetch is over 3 hours
// old, the first three shown, hidden with no data, never fetched once the user removed it.
#include "feature/presenters/HomeBrief.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace pbr {
namespace {

constexpr int64_t kNow = 1'800'000'000'000; // unix ms
constexpr int64_t kHour = 60LL * 60 * 1000;

HomeBriefCache CacheOf(const std::string& feed, const int64_t fetched_at_ms, const size_t count) {
  HomeBriefCache cache;
  cache.feed = feed;
  cache.fetched_at_ms = fetched_at_ms;
  for (size_t i = 0; i < count; ++i) {
    cache.items.push_back({.title = "headline " + std::to_string(i + 1), .url = "https://example.com/" + std::to_string(i + 1)});
  }
  return cache;
}

TEST(HomeBriefTest, ReadsTheArticleFeedAnswer) {
  const auto items = ParseHomeBriefArticles(R"({"articles":[
      {"id":"1","title":"Oil prices ease","content":"Long text","link_to":"https://WWW.Example.com:443/a?b","created_at":1800000000},
      {"id":"2","title":"","content":"Only text here","link_to":"http://plain.example.org/x","created_at":1800000000123},
      {"id":"3","title":"","content":""}]})");
  ASSERT_TRUE(items.has_value());
  ASSERT_EQ(items->size(), 2u) << "an article with neither title nor text is skipped";
  EXPECT_EQ((*items)[0].title, "Oil prices ease");
  EXPECT_EQ((*items)[0].category, "example.com");
  EXPECT_EQ((*items)[0].url, "https://WWW.Example.com:443/a?b");
  EXPECT_EQ((*items)[0].published_at_ms, 1800000000000);
  EXPECT_EQ((*items)[1].title, "Only text here") << "no title: the text stands in";
  EXPECT_TRUE((*items)[1].url.empty()) << "only https links are kept";
  EXPECT_EQ((*items)[1].published_at_ms, 1800000000123);
}

TEST(HomeBriefTest, AnAnswerThatIsNotAFeedKeepsTheOldCache) {
  EXPECT_FALSE(ParseHomeBriefArticles("tool failed: timeout").has_value());
  EXPECT_FALSE(ParseHomeBriefArticles(R"({"error":"nope"})").has_value());
  const auto empty = ParseHomeBriefArticles(R"({"articles":[]})");
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->empty());
}

TEST(HomeBriefTest, StaleAfterThreeHoursOrForAnotherLanguage) {
  EXPECT_TRUE(HomeBriefStale(HomeBriefCache{}, "cn", kNow)) << "never fetched";
  EXPECT_FALSE(HomeBriefStale(CacheOf("cn", kNow - 3 * kHour, 3), "cn", kNow)) << "exactly 3 h: still fresh";
  EXPECT_TRUE(HomeBriefStale(CacheOf("cn", kNow - 3 * kHour - 1, 3), "cn", kNow));
  EXPECT_FALSE(HomeBriefStale(CacheOf("cn", kNow - kHour, 3), "cn", kNow));
  EXPECT_TRUE(HomeBriefStale(CacheOf("cn", kNow - kHour, 3), "en", kNow)) << "the UI language changed";
  EXPECT_TRUE(HomeBriefStale(CacheOf("cn", kNow + kHour, 3), "cn", kNow)) << "a clock that went back";
}

TEST(HomeBriefTest, ShowsTheFirstThreeOfItsOwnLanguage) {
  const std::vector<HomeBriefItem> top = HomeBriefTop(CacheOf("en", kNow - 5 * kHour, 10), "en");
  ASSERT_EQ(top.size(), 3u) << "an old cache still shows while the new one is fetched";
  EXPECT_EQ(top[0].title, "headline 1");
  EXPECT_EQ(top[2].title, "headline 3");
  EXPECT_EQ(HomeBriefTop(CacheOf("en", kNow, 2), "en").size(), 2u);
  EXPECT_TRUE(HomeBriefTop(CacheOf("cn", kNow, 5), "en").empty()) << "another language's headlines are not shown";
  EXPECT_TRUE(HomeBriefTop(HomeBriefCache{}, "en").empty()) << "no data: the widget hides";
}

TEST(HomeBriefTest, SkipsHeadlinesWithoutAPageToOpen) {
  HomeBriefCache cache = CacheOf("en", kNow, 4);
  cache.items[0].url.clear();
  const std::vector<HomeBriefItem> top = HomeBriefTop(cache, "en");
  ASSERT_EQ(top.size(), 3u);
  EXPECT_EQ(top[0].title, "headline 2") << "every row opens its page; the briefs chat is See all's";
  EXPECT_EQ(top[2].title, "headline 4");
}

TEST(HomeBriefTest, FetchesOnlyWhenOnStaleIdleAndNotJustFailed) {
  const HomeBriefCache stale = CacheOf("cn", kNow - 4 * kHour, 3);
  const HomeBriefCache fresh = CacheOf("cn", kNow - kHour, 3);
  EXPECT_TRUE(ShouldFetchHomeBrief(true, false, 0, stale, "cn", kNow));
  EXPECT_TRUE(ShouldFetchHomeBrief(true, false, 0, HomeBriefCache{}, "cn", kNow)) << "first open";
  EXPECT_FALSE(ShouldFetchHomeBrief(true, false, 0, fresh, "cn", kNow)) << "fetched under 3 h ago";
  EXPECT_FALSE(ShouldFetchHomeBrief(true, true, 0, stale, "cn", kNow)) << "one fetch at a time";
  EXPECT_FALSE(ShouldFetchHomeBrief(true, false, kNow - 60'000, stale, "cn", kNow)) << "failed a minute ago";
  EXPECT_TRUE(ShouldFetchHomeBrief(true, false, kNow - kHomeBriefRetryMs, stale, "cn", kNow));
}

TEST(HomeBriefTest, ARemovedWidgetIsNeverFetched) {
  EXPECT_FALSE(ShouldFetchHomeBrief(false, false, 0, HomeBriefCache{}, "cn", kNow));
  EXPECT_FALSE(ShouldFetchHomeBrief(false, false, 0, CacheOf("cn", kNow - 10 * kHour, 3), "cn", kNow));
}

TEST(HomeBriefTest, TheCacheRoundTripsInTheProfileDirectory) {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "pp_home_brief_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  EXPECT_TRUE(LoadHomeBriefCache(dir.string()).items.empty()) << "no file: an empty cache";

  HomeBriefCache cache;
  cache.fetched_at_ms = kNow;
  cache.feed = "en";
  cache.items = {{.title = "A", .category = "example.com", .url = "https://example.com/a", .published_at_ms = kNow - kHour},
                 {.title = "B"}};
  ASSERT_TRUE(SaveHomeBriefCache(dir.string(), cache));
  EXPECT_TRUE(std::filesystem::exists(dir / "home_brief.json"));
  const HomeBriefCache back = LoadHomeBriefCache(dir.string());
  EXPECT_EQ(back.fetched_at_ms, kNow);
  EXPECT_EQ(back.feed, "en");
  EXPECT_EQ(back.items, cache.items);

  std::ofstream(dir / "home_brief.json", std::ios::trunc) << "{not json";
  EXPECT_TRUE(LoadHomeBriefCache(dir.string()).items.empty()) << "a damaged file reads as no data";
  std::filesystem::remove_all(dir);
}

TEST(HomeBriefTest, TheLineUnderAHeadlineNamesTheSiteAndTheTime) {
  EXPECT_EQ(HomeBriefMeta({.title = "t", .category = "example.com"}, kNow), "example.com");
  EXPECT_TRUE(HomeBriefMeta({.title = "t"}, kNow).empty());
  const std::string both = HomeBriefMeta({.title = "t", .category = "example.com", .published_at_ms = kNow - 60'000}, kNow);
  EXPECT_EQ(both.rfind("example.com · ", 0), 0u) << both;
}

} // namespace
} // namespace pbr
