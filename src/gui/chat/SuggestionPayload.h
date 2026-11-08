#pragma once

#include "common/ValueJson.h"

#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** The article feed for a UI language: "cn" for Chinese ("zh…"), "en" for everything else. */
inline std::string ArticleFeedName(const std::string& ui_language) {
  return ui_language.rfind("zh", 0) == 0 ? "cn" : "en";
}

/** The arguments of the article feed call (blog_articles): the chip's, and Home's Today's briefs widget's. */
inline Object ArticleFeedArguments(const std::string& ui_language) {
  const std::string feed = ArticleFeedName(ui_language);
  Object args;
  args.set("brf_domain", feed);
  args.set("brf_language", feed);
  args.set("size", static_cast<int64_t>(10));
  return args;
}

/**
 * Tool payload of a home suggestion chip that runs a device function directly instead of asking the AI
 * (ids: "find_someone", "articles"). nullopt for any other id. The article feed follows the UI language:
 * Chinese ("zh…") reads the cn feed, everything else the en feed.
 */
inline std::optional<std::string> SuggestionPayload(const std::string& id, const std::string& ui_language) {
  Object payload;
  if (id == "find_someone") {
    // An empty query lists the public directory (the same call the AI made for a generic "find someone").
    payload.set("tool", "search_people");
    payload.set("query", "");
    return DumpJson(payload);
  }
  if (id == "articles") {
    payload = ArticleFeedArguments(ui_language);
    payload.set("tool", "blog_articles");
    return DumpJson(payload);
  }
  return std::nullopt;
}

} // namespace pbr
