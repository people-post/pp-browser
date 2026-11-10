#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace pbr {

struct ConversationSummary {
  int schema_version = 1;
  std::string text;
  int version = 0;
  /** Messages at or below this display_order are represented by summary (D040). */
  std::optional<int64_t> compacted_through_display_order;
  int64_t updated_at = 0;
};

} // namespace pbr
