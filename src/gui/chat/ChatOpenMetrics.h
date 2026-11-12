#pragma once

#include "lib/ui/ChatWidgetTypes.h"

#include <cstddef>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

namespace pbr {

/** The row draws a picture bubble: a picture clip with `<img class="chat-attachment-image">` (a video poster does not). */
inline bool IsPictureBubbleRow(const MessageDisplayRow& row) {
  const std::string_view rml(row.content_rml.data(), row.content_rml.size());
  return rml.find("class=\"chat-attachment-image\"") != std::string_view::npos;
}

/** How many of the rows are picture bubbles (the `pictures` field of chat.open / chat.window). */
inline size_t CountPictureBubbleRows(const std::vector<MessageDisplayRow>& rows) {
  size_t count = 0;
  for (const MessageDisplayRow& row : rows) {
    if (IsPictureBubbleRow(row)) {
      ++count;
    }
  }
  return count;
}

/**
 * PROBE (measurement only): the `src` of every `<img class="chat-attachment-image">` in the rows, unescaped (the
 * entities StructuredTextParser::EscapeText writes), in row order.
 */
inline std::vector<std::string> PictureBubbleSources(const std::vector<MessageDisplayRow>& rows) {
  static constexpr std::string_view kTag = "<img class=\"chat-attachment-image\" src=\"";
  std::vector<std::string> out;
  for (const MessageDisplayRow& row : rows) {
    const std::string_view rml(row.content_rml.data(), row.content_rml.size());
    size_t at = 0;
    while ((at = rml.find(kTag, at)) != std::string_view::npos) {
      at += kTag.size();
      const size_t end = rml.find('"', at);
      if (end == std::string_view::npos) {
        break;
      }
      std::string src(rml.substr(at, end - at));
      static constexpr std::pair<std::string_view, char> kEntities[] = {
          {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&amp;", '&'}};
      for (const auto& [from, to] : kEntities) {
        for (size_t pos = 0; (pos = src.find(from, pos)) != std::string::npos; ++pos) {
          src.replace(pos, from.size(), 1, to);
        }
      }
      out.push_back(std::move(src));
      at = end;
    }
  }
  return out;
}

} // namespace pbr
