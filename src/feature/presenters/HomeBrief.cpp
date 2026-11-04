#include "feature/presenters/HomeBrief.h"

#include "feature/presenters/SessionListText.h"

#include "common/BoundedRead.h"
#include "common/PlatformLimits.h"
#include "common/ValueJson.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace pbr {

namespace {

constexpr const char* kCacheFile = "home_brief.json";

bool IsHttpsUrl(const std::string& url) {
  return url.size() > 8 && url.compare(0, 8, "https://") == 0;
}

/** The site of an https link for the line under a headline: lower case, no "www.", no userinfo or port. */
std::string SiteOf(const std::string& url) {
  if (!IsHttpsUrl(url)) {
    return {};
  }
  std::string host = url.substr(8, url.find_first_of("/?#\\", 8) - 8);
  if (const size_t at = host.rfind('@'); at != std::string::npos) {
    host.erase(0, at + 1);
  }
  if (const size_t colon = host.find(':'); colon != std::string::npos) {
    host.erase(colon);
  }
  std::transform(host.begin(), host.end(), host.begin(),
                 [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (host.compare(0, 4, "www.") == 0) {
    host.erase(0, 4);
  }
  return host;
}

} // namespace

std::optional<std::vector<HomeBriefItem>> ParseHomeBriefArticles(const std::string& json) {
  const std::optional<Object> doc = TryParseObject(json);
  const Array* articles = doc ? doc->getArray("articles") : nullptr;
  if (!articles) {
    return std::nullopt;
  }
  std::vector<HomeBriefItem> items;
  for (const Value& value : articles->elements) {
    const Object* article = asObject(value);
    if (!article) {
      continue;
    }
    const std::string title = article->getString("title").value_or("");
    const std::string content = article->getString("content").value_or("");
    if (title.empty() && content.empty()) {
      continue;
    }
    HomeBriefItem item;
    item.title = title.empty() ? content : title;
    const std::string link = article->getString("link_to").value_or("");
    if (IsHttpsUrl(link)) {
      item.url = link;
      item.category = SiteOf(link);
    }
    // The feed sends unix seconds or milliseconds.
    const int64_t created = article->getIf<int64_t>("created_at").value_or(0);
    item.published_at_ms = created <= 0 ? 0 : (created > 100000000000LL ? created : created * 1000);
    items.push_back(std::move(item));
  }
  return items;
}

bool HomeBriefStale(const HomeBriefCache& cache, const std::string& feed, const int64_t now_ms) {
  return cache.fetched_at_ms <= 0 || cache.feed != feed || now_ms - cache.fetched_at_ms > kHomeBriefMaxAgeMs ||
         now_ms < cache.fetched_at_ms;
}

bool ShouldFetchHomeBrief(const bool widget_on, const bool fetching, const int64_t last_failed_ms,
                          const HomeBriefCache& cache, const std::string& feed, const int64_t now_ms) {
  if (!widget_on || fetching || !HomeBriefStale(cache, feed, now_ms)) {
    return false;
  }
  return last_failed_ms <= 0 || now_ms - last_failed_ms >= kHomeBriefRetryMs || now_ms < last_failed_ms;
}

std::vector<HomeBriefItem> HomeBriefTop(const HomeBriefCache& cache, const std::string& feed) {
  if (cache.feed != feed) {
    return {};
  }
  const size_t count = std::min(cache.items.size(), kHomeBriefShown);
  return {cache.items.begin(), cache.items.begin() + static_cast<std::ptrdiff_t>(count)};
}

std::string HomeBriefMeta(const HomeBriefItem& item, const int64_t now_ms) {
  const std::string when = SessionDateLabel(item.published_at_ms, now_ms);
  if (item.category.empty()) {
    return when;
  }
  return when.empty() ? item.category : item.category + " · " + when;
}

HomeBriefCache LoadHomeBriefCache(const std::string& profile_dir) {
  HomeBriefCache cache;
  if (profile_dir.empty()) {
    return cache;
  }
  std::ifstream in(std::filesystem::path(profile_dir) / kCacheFile);
  if (!in) {
    return cache;
  }
  const auto text = ReadBounded(in, kMaxProfileJsonFileBytes);
  const std::optional<Object> root = text ? TryParseObject(*text) : std::nullopt;
  if (!root || root->getIf<int64_t>("schema_version").value_or(0) != HomeBriefCache::kSchemaVersion) {
    return cache;
  }
  cache.fetched_at_ms = root->getIf<int64_t>("fetched_at_ms").value_or(0);
  cache.feed = root->getString("feed").value_or("");
  if (const Array* items = root->getArray("items")) {
    for (const Value& value : items->elements) {
      const Object* entry = asObject(value);
      if (!entry) {
        continue;
      }
      HomeBriefItem item;
      item.title = entry->getString("title").value_or("");
      item.category = entry->getString("category").value_or("");
      item.url = entry->getString("url").value_or("");
      item.published_at_ms = entry->getIf<int64_t>("time_ms").value_or(0);
      if (!item.title.empty()) {
        cache.items.push_back(std::move(item));
      }
    }
  }
  return cache;
}

Roe<void> SaveHomeBriefCache(const std::string& profile_dir, const HomeBriefCache& cache) {
  if (profile_dir.empty()) {
    return Error("Empty profile data dir");
  }
  std::vector<Value> items;
  for (const HomeBriefItem& item : cache.items) {
    Object entry;
    entry.set("title", item.title);
    entry.set("category", item.category);
    entry.set("time_ms", item.published_at_ms);
    entry.set("url", item.url);
    items.push_back(ObjectValue(std::move(entry)));
  }
  Object root;
  root.set("schema_version", static_cast<int64_t>(HomeBriefCache::kSchemaVersion));
  root.set("fetched_at_ms", cache.fetched_at_ms);
  root.set("feed", cache.feed);
  root.set("items", ArrayValue(std::move(items)));
  std::ofstream out(std::filesystem::path(profile_dir) / kCacheFile, std::ios::trunc);
  if (!out) {
    return Error("Failed to write the Home brief cache");
  }
  out << DumpJson(root, 2);
  return {};
}

} // namespace pbr
