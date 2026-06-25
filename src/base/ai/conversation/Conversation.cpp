#include "base/ai/conversation/Conversation.h"

#include <algorithm>

namespace pbr {

void Conversation::StartNewConversation() {
  std::lock_guard lock(mutex_);
  conversation_id_ = "conv_" + std::to_string(next_conversation_id_++);
  entries_.clear();
  summary_.reset();
  next_entry_id_ = 1;
}

std::string Conversation::MakeEntryId() {
  return "entry_" + std::to_string(next_entry_id_++);
}

TranscriptEntry& Conversation::AppendUser(const std::string& user_text,
                                          std::optional<std::string> user_payload) {
  std::lock_guard lock(mutex_);
  if (conversation_id_.empty()) {
    conversation_id_ = "conv_" + std::to_string(next_conversation_id_++);
  }

  const int turn_index = static_cast<int>(std::count_if(entries_.begin(), entries_.end(),
                                                        [](const TranscriptEntry& entry) {
                                                          return entry.assistant_raw.has_value();
                                                        })) +
                         1;

  entries_.push_back(TranscriptEntry{
      .id = MakeEntryId(),
      .turn_index = turn_index,
      .user_text = user_text,
      .user_payload = std::move(user_payload),
  });
  return entries_.back();
}

bool Conversation::CompleteTurn(const std::string& entry_id, const std::string& assistant_raw) {
  std::lock_guard lock(mutex_);
  for (TranscriptEntry& entry : entries_) {
    if (entry.id == entry_id) {
      entry.assistant_raw = assistant_raw;
      return true;
    }
  }
  return false;
}

bool Conversation::SetAssistantDisplay(const std::string& entry_id, const std::string& assistant_rml,
                                       std::vector<TranscriptChatAction> chat_actions) {
  std::lock_guard lock(mutex_);
  for (TranscriptEntry& entry : entries_) {
    if (entry.id == entry_id) {
      entry.assistant_rml = assistant_rml;
      entry.chat_actions = std::move(chat_actions);
      return true;
    }
  }
  return false;
}

void Conversation::SetSummary(ConversationSummary summary) {
  std::lock_guard lock(mutex_);
  summary_ = std::move(summary);
}

int Conversation::CompletedTurnCount() const {
  std::lock_guard lock(mutex_);
  return static_cast<int>(std::count_if(entries_.begin(), entries_.end(), [](const TranscriptEntry& entry) {
    return entry.assistant_raw.has_value();
  }));
}

std::vector<TranscriptEntry> Conversation::CompletedEntries() const {
  std::lock_guard lock(mutex_);
  std::vector<TranscriptEntry> completed;
  completed.reserve(entries_.size());
  for (const TranscriptEntry& entry : entries_) {
    if (entry.assistant_raw.has_value()) {
      completed.push_back(entry);
    }
  }
  return completed;
}

} // namespace pbr
