#pragma once

#include "lib/ui/ChatWidgetTypes.h"

#include <cstddef>
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

} // namespace pbr
