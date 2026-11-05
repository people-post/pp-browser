#pragma once

#include "common/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The Today's briefs widget on the phone's Home (DESIGN-pp-ui 8.16; Kenneth 2026-11-03: fetch again when Home opens
 * and the last fetch is over 3 hours old, show the first 3, hide it with no data). The headlines come from the same
 * article feed the "Latest briefs" chip shows; only general headlines are kept, so the cache is not encrypted.
 */
struct HomeBriefItem {
  std::string title;
  /** What the line under the title names: the article's site (the feed carries no category name). */
  std::string category;
  /** https link of the article; empty when it has none. */
  std::string url;
  /** When it was published, unix ms; 0 unknown. */
  int64_t published_at_ms = 0;

  bool operator==(const HomeBriefItem&) const = default;
};

/** profiles/{id}/home_brief.json (docs/contracts/DATA_LAYOUT.md). */
struct HomeBriefCache {
  static constexpr int kSchemaVersion = 1;
  /** When the feed last answered, unix ms; 0 never. */
  int64_t fetched_at_ms = 0;
  /** Which feed the items are from: "cn" or "en" (follows the UI language). */
  std::string feed;
  std::vector<HomeBriefItem> items;
};

inline constexpr int64_t kHomeBriefMaxAgeMs = 3LL * 60 * 60 * 1000;
/** After a failed fetch, Home asks again no sooner than this (tab switches must not hammer the server). */
inline constexpr int64_t kHomeBriefRetryMs = 10LL * 60 * 1000;
inline constexpr size_t kHomeBriefShown = 3;

/**
 * The headlines of an article feed answer ({"articles":[...]}, what the blog_articles tool returns), in feed order;
 * an article with neither title nor text is skipped, one with no title shows its text. nullopt when `json` is not
 * such a document (an error text, a broken answer): the old cache then stays.
 */
std::optional<std::vector<HomeBriefItem>> ParseHomeBriefArticles(const std::string& json);

/** The cache is for another feed (the UI language changed), was never filled, or is over 3 hours old. */
bool HomeBriefStale(const HomeBriefCache& cache, const std::string& feed, int64_t now_ms);

/**
 * Whether Home should fetch the briefs now: the widget is on (a removed widget is never fetched), no fetch is
 * running, the cache is stale, and the last failed try (0: none) is older than kHomeBriefRetryMs.
 */
bool ShouldFetchHomeBrief(bool widget_on, bool fetching, int64_t last_failed_ms, const HomeBriefCache& cache,
                          const std::string& feed, int64_t now_ms);

/** The first kHomeBriefShown items with a link when the cache is for `feed` (even when old); none otherwise. */
std::vector<HomeBriefItem> HomeBriefTop(const HomeBriefCache& cache, const std::string& feed);

/** The line under a headline: "site · 08:00" (the chats list's date style); either part may be missing. */
std::string HomeBriefMeta(const HomeBriefItem& item, int64_t now_ms);

/** The cache in `profile_dir`; an empty cache when the file is missing or unreadable. */
HomeBriefCache LoadHomeBriefCache(const std::string& profile_dir);
Roe<void> SaveHomeBriefCache(const std::string& profile_dir, const HomeBriefCache& cache);

} // namespace pbr
