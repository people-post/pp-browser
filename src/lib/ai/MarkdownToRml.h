#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace pbr {

struct MarkdownRml {
  std::string rml;                // well-formed RML fragment
  std::vector<std::string> links; // https URLs, referenced from rml by index (deduplicated)
};

// Converts untrusted Markdown (LLM output) into an RML fragment. All text is escaped with
// StructuredTextParser::EscapeText; only https links are emitted (as click events that refer to
// `links` by index); images render as their alt text. Every prefix of a document yields a
// well-formed fragment, so it is safe to call repeatedly while an answer streams in.
MarkdownRml MarkdownToRml(std::string_view markdown);

} // namespace pbr
