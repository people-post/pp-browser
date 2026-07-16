#pragma once

#include "base/ai/conversation/ConversationTypes.h"
#include "base/messaging/ThreadMemoryTypes.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

class Conversation {
public:
  void StartNewConversation();

  const std::string& ConversationId() const { return conversation_id_; }

  TranscriptEntry& AppendUser(const std::string& user_text,
                              std::optional<std::string> user_payload = std::nullopt);
  bool CompleteTurn(const std::string& entry_id, const std::string& assistant_raw);
  bool SetAssistantDisplay(const std::string& entry_id, const std::string& assistant_rml,
                           std::vector<TranscriptChatAction> chat_actions);

  const std::vector<TranscriptEntry>& Entries() const { return entries_; }
  const std::optional<ConversationSummary>& Summary() const { return summary_; }
  void SetSummary(ConversationSummary summary);

  int CompletedTurnCount() const;

  std::vector<TranscriptEntry> CompletedEntries() const;

private:
  std::string MakeEntryId();

  mutable std::mutex mutex_;
  std::string conversation_id_;
  std::vector<TranscriptEntry> entries_;
  std::optional<ConversationSummary> summary_;
  int next_entry_id_ = 1;
  int next_conversation_id_ = 1;
};

} // namespace pbr
