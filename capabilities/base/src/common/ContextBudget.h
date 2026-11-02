#pragma once

#include <cstdint>
#include <optional>

namespace pbr {

struct ContextBudget {
  int max_turn_pairs = 6;
  int max_recent_chars = 6000;
  int max_input_tokens = 8000;
  double token_estimate_margin = 0.85;
  /** Runtime cap for summary injection; align with kMaxSummaryBytes (D040). */
  int max_summary_chars = 8192;
};

inline ContextBudget DefaultContextBudget() {
  return ContextBudget{};
}

} // namespace pbr
