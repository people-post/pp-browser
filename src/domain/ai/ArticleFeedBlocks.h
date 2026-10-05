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
  std::string empty = "No briefs right now.";
  std::string open = "[View details]";
  std::string more = "Load more";
  std::string more_message = "Load more briefs";
};

struct ArticleFeedBuildOptions {
  ArticleFeedLabels labels;
  /** Arguments of the tool call that produced the result; a "load more" payload repeats them with before_id. */
  Object call_arguments;
  /** The feed tool that produced the result; "load more" calls the same one. */
  std::string tool_name = "blog_articles";
  size_t max_items = 10;
};

/**
 * The payload of an MCP tool result. McpToolAdapter returns the whole MCP `result` object,
 * {"content":[{"type":"text","text":"<json>"}]}; this returns that inner text when present, and `raw`
 * unchanged otherwise (plain JSON, mock results).
 */
std::string UnwrapMcpTextResult(const std::string& raw);

/**
 * Builds the blocks JSON ({"blocks":[...]}) for a blog_articles tool result without a model: a short intro
 * paragraph plus a long_list (title or brief sentence, "host · time" meta, an "open_url" action for https
 * links). Empty string when `raw_json` is not an {"articles":[...]} document.
 */
std::string BuildArticleFeedBlocksJson(const std::string& raw_json, const ArticleFeedBuildOptions& options = {});

} // namespace pbr
