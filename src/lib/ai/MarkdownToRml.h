#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace pbr {

/** The link title that marks a news item (`[title](https://… "pp:news")`); only brief_AI's lists and our own
 *  "details" link carry it (brief_AI/docs/contracts/pp-client.md section 15). */
inline constexpr std::string_view kNewsLinkTitle = "pp:news";
/** The class of a news item's span, and the one added to it once the item was read. */
inline constexpr std::string_view kNewsLinkClass = "news-link";
inline constexpr std::string_view kNewsReadClass = "news-read";

struct MarkdownLink {
  std::string url;         // https
  bool news = false;       // a news item (title `pp:news`, honoured only when MarkdownOptions::news_links)
  std::string label;       // the link's text as shown (news items only)
  size_t news_ordinal = 0; // its place among the document's news items (news items only)
};

struct MarkdownOptions {
  /** Honour the `pp:news` title: only for the AI assistant's own answers in an AI conversation. */
  bool news_links = false;
  /** false: no link is tappable; a link shows as its text only and a bare URL as plain text (an article's digest). */
  bool links = true;
};

struct MarkdownRml {
  std::string rml;                 // well-formed RML fragment
  std::vector<MarkdownLink> links; // referenced from rml by index (deduplicated by URL and kind)
};

// Converts untrusted Markdown (LLM output) into an RML fragment. All text is escaped with
// StructuredTextParser::EscapeText; only https links are emitted (as click events that refer to
// `links` by index); images render as their alt text. A link title is never shown; `pp:news` makes
// the link a news item when `options.news_links`. Every prefix of a document yields a
// well-formed fragment, so it is safe to call repeatedly while an answer streams in.
MarkdownRml MarkdownToRml(std::string_view markdown, const MarkdownOptions& options = {});

} // namespace pbr
