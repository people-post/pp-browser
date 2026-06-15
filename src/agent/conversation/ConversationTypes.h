#pragma once

#include "agent/LlmClient.h"

#include <optional>
#include <string>
#include <vector>

namespace pbr {

struct TranscriptChatAction {
  std::string label;
  std::string message;
  std::optional<std::string> payload;
};

struct TranscriptEntry {
  std::string id;
  int turn_index = 0;
  std::string user_text;
  std::optional<std::string> user_payload;
  std::optional<std::string> assistant_raw;
  std::optional<std::string> assistant_rml;
  std::vector<TranscriptChatAction> chat_actions;
};

struct ConversationSummary {
  std::string text;
  int version = 0;
};

struct ContextBudget {
  int max_turn_pairs = 6;
  int max_recent_chars = 6000;
  int max_input_tokens = 8000;
  double token_estimate_margin = 0.85;
  int max_summary_chars = 2000;
};

inline ContextBudget DefaultContextBudget() {
  return ContextBudget{};
}

struct ContextProvenance {
  int estimated_input_tokens = 0;
  int trimmed_turn_count = 0;
  std::vector<std::string> included_entry_ids;
  bool summary_included = false;
};

struct ContextBuildResult {
  std::vector<ChatMessage> messages;
  ContextProvenance provenance;
};

struct TurnSnapshot {
  std::string entry_id;
  int turn_index = 0;
  std::vector<ChatMessage> messages;
  ContextProvenance provenance;
};

} // namespace pbr
