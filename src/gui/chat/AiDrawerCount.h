#pragma once

#include "foundation/i18n/LocalizationService.h"

#include <string>

namespace pbr {

/**
 * The phone AI drawer's count line ("23 conversations"): every conversation the list holds, never the search box's
 * matches (ChatController::ApplySessionList passes the unfiltered row count). Empty when there are none, so the strip
 * shows nothing.
 */
inline std::string AiDrawerCountLabel(const size_t all_conversations, const bool archived = false) {
  if (all_conversations == 0) {
    return {};
  }
  // The archived list says so ("23 archived conversations"): it is not the whole history.
  const char* key = archived ? (all_conversations == 1 ? "ai_drawer.archived_count_one" : "ai_drawer.archived_count")
                             : (all_conversations == 1 ? "ai_drawer.count_one" : "ai_drawer.count");
  return Tr(key, {{"count", std::to_string(all_conversations)}});
}

} // namespace pbr
