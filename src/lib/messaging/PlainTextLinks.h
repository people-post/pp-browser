#pragma once

#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace pbr {

/**
 * https links in a plain-text chat message, so a bubble can make them tappable. Only https; the text is
 * otherwise untouched. A link written right after one of the "view details" labels (what the article
 * Copy / Share actions produce: "<article>\n【查看详情】https://…") is shown as that label alone.
 * The click handler finds the same links again from the stored text, by index, so no URL ever sits in
 * the bubble's markup.
 */
struct PlainTextLink {
  size_t begin = 0;      // first byte of the span in the text (the label when there is one)
  size_t end = 0;        // one past the last byte of the URL
  std::string url;
  std::string display;   // the label, or the URL itself
};

/** The labels recognised before a link; the same strings as locale key feed.result.open. */
inline constexpr std::array<std::string_view, 2> kViewDetailsLabels = {"\xE3\x80\x90\xE6\x9F\xA5\xE7\x9C\x8B\xE8\xAF\xA6\xE6\x83\x85\xE3\x80\x91",
                                                                       "[View details]"};

inline std::vector<PlainTextLink> FindPlainTextLinks(const std::string_view text) {
  constexpr std::string_view kScheme = "https://";
  std::vector<PlainTextLink> links;
  for (size_t pos = text.find(kScheme); pos != std::string_view::npos; pos = text.find(kScheme, pos)) {
    // Not the tail of another word ("xhttps://"): a link starts the text or follows a non-ASCII-word byte.
    const bool word_before = pos > 0 && (std::isalnum(static_cast<unsigned char>(text[pos - 1])) != 0);
    size_t end = pos + kScheme.size();
    while (end < text.size() && static_cast<unsigned char>(text[end]) > 0x20 && text[end] != '<' && text[end] != '>' &&
           text[end] != '"' && static_cast<unsigned char>(text[end]) < 0x80) {
      ++end; // ASCII only: CJK punctuation right after a link ends it
    }
    while (end > pos + kScheme.size() && std::string_view(".,;:!?)]}'").find(text[end - 1]) != std::string_view::npos) {
      --end; // sentence punctuation is not part of the link
    }
    if (word_before || end == pos + kScheme.size()) {
      pos = end;
      continue;
    }
    PlainTextLink link;
    link.begin = pos;
    link.end = end;
    link.url = std::string(text.substr(pos, end - pos));
    link.display = link.url;
    for (const std::string_view label : kViewDetailsLabels) {
      if (pos >= label.size() && text.substr(pos - label.size(), label.size()) == label) {
        link.begin = pos - label.size();
        link.display = std::string(label);
        break;
      }
    }
    links.push_back(std::move(link));
    pos = end;
  }
  return links;
}

} // namespace pbr
