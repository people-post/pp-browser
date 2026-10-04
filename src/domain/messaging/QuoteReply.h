#pragma once

#include <optional>
#include <string>

namespace pbr {

/**
 * A reply that quotes another message travels as plain text: the reply, a blank line, then the quoted
 * message with "> " in front of each line. Clients that know the form show the tail as a quote block;
 * anything else still shows readable text. No wire or stored field is involved.
 */
struct QuoteReplyParts {
  std::string reply;
  std::string quote;
};

/** `quoted` is cut to `max_quote_chars` code points (an ellipsis marks the cut). */
inline std::string ComposeQuoteReply(const std::string& reply, const std::string& quoted, const size_t max_quote_chars = 300) {
  std::string out = reply + "\n\n> ";
  size_t chars = 0;
  for (size_t i = 0; i < quoted.size(); ++i) {
    const auto byte = static_cast<unsigned char>(quoted[i]);
    if ((byte & 0xC0) != 0x80 && ++chars > max_quote_chars) {
      out += "\xE2\x80\xA6";
      break;
    }
    out += quoted[i];
    if (quoted[i] == '\n') {
      out += "> ";
    }
  }
  return out;
}

/** The inverse: nullopt unless `text` is a non-empty reply followed by a blank line and only "> " lines. */
inline std::optional<QuoteReplyParts> SplitQuoteReply(const std::string& text) {
  const size_t split = text.find("\n\n> ");
  if (split == std::string::npos || split == 0) {
    return std::nullopt;
  }
  QuoteReplyParts parts;
  parts.reply = text.substr(0, split);
  size_t pos = split + 2;
  while (pos < text.size()) {
    if (text.compare(pos, 2, "> ") != 0) {
      return std::nullopt;
    }
    const size_t end = text.find('\n', pos);
    const size_t stop = end == std::string::npos ? text.size() : end;
    if (!parts.quote.empty()) {
      parts.quote += '\n';
    }
    parts.quote += text.substr(pos + 2, stop - (pos + 2));
    pos = end == std::string::npos ? text.size() : end + 1;
  }
  return parts;
}

} // namespace pbr
