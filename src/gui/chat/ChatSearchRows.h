#pragma once

#include "lib/ui/ChatWidgetTypes.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace pbr {

/** Search inside the chat: tints the rows of `hit_ids`, the one of `current_id` darker. Other rows are cleared. */
inline void MarkSearchRows(std::vector<MessageDisplayRow>& rows, const std::unordered_set<std::string>& hit_ids,
                           const std::string& current_id) {
  for (MessageDisplayRow& row : rows) {
    const std::string id = row.message_id.c_str();
    row.search_hit = hit_ids.count(id) > 0;
    row.search_current = row.search_hit && id == current_id;
  }
}

} // namespace pbr
