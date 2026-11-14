#pragma once

#include "foundation/i18n/LocalizationService.h"

#include <string>

namespace pbr {

/**
 * The phone AI drawer's count line ("23 conversations"): every conversation the list holds, never the search box's
 * matches (ChatController::ApplySessionList passes the unfiltered row count). Empty when there are none, so the strip
 * shows nothing.
 */
inline std::string AiDrawerCountLabel(const size_t all_conversations) {
  if (all_conversations == 0) {
    return {};
  }
  return Tr(all_conversations == 1 ? "ai_drawer.count_one" : "ai_drawer.count",
            {{"count", std::to_string(all_conversations)}});
}

} // namespace pbr
