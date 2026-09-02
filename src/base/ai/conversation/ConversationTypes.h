#pragma once

#include "base/ai/LlmClient.h"
#include "common/thread/ContextBudget.h"
#include "common/ChatActionTypes.h"

#include <optional>
#include <string>
#include <vector>

namespace pbr {

struct TranscriptEntry {
  std::string id;
  int turn_index = 0;
  std::string user_text;
  std::optional<std::string> user_payload;
  std::optional<std::string> assistant_raw;
  std::optional<std::string> assistant_rml;
  std::vector<TranscriptChatAction> chat_actions;
};

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
