#pragma once

#include "common/ValueJson.h"

#include <optional>
#include <string>

namespace pbr {

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
    const std::string feed = ui_language.rfind("zh", 0) == 0 ? "cn" : "en";
    payload.set("tool", "blog_articles");
    payload.set("brf_domain", feed);
    payload.set("brf_language", feed);
    payload.set("size", static_cast<int64_t>(10));
    return DumpJson(payload);
  }
  return std::nullopt;
}

} // namespace pbr
