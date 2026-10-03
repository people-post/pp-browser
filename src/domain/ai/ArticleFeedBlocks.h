#pragma once

#include "common/ValueJson.h"

#include <cstddef>
#include <optional>
#include <string>

namespace pbr {

/** User-visible strings of the article feed blocks. Defaults are English; see domain/ai/LocalizedLabels.h. */
struct ArticleFeedLabels {
  std::string intro = "Latest from Brief ({count}):";
  std::string title = "Latest briefs";
  std::string empty = "No articles right now.";
  std::string open = "Open";
  std::string more = "Load more";
  std::string more_message = "Load more articles";
};

struct ArticleFeedBuildOptions {
  ArticleFeedLabels labels;
  /** Arguments of the tool call that produced the result; a "load more" payload repeats them with before_id. */
  Object call_arguments;
  size_t max_items = 10;
};

/**
 * Builds the blocks JSON ({"blocks":[...]}) for a blog_articles tool result without a model: a short intro
 * paragraph plus a long_list (title or brief sentence, "host · time" meta, an "open_url" action for https
 * links). Empty string when `raw_json` is not an {"articles":[...]} document.
 */
std::string BuildArticleFeedBlocksJson(const std::string& raw_json, const ArticleFeedBuildOptions& options = {});

} // namespace pbr
