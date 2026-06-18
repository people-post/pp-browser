#pragma once

#include "agent/conversation/ConversationTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

inline constexpr const char* kLocalSelfContactId = "local:self";
inline constexpr const char* kAiAssistantContactId = "ai:assistant";

enum class ThreadKind { Ai, Direct, Group };

enum class MessageDelivery { Local, Pending, Relayed, Failed };

struct Thread {
  std::string id;
  ThreadKind kind = ThreadKind::Ai;
  std::string title;
  std::vector<std::string> participant_contact_ids;
  int64_t updated_at = 0;
  int unread_count = 0;
  std::string preview;
};

struct ThreadMessage {
  std::string id;
  std::string thread_id;
  std::string sender_contact_id;
  std::string text;
  std::optional<std::string> content_rml;
  std::vector<TranscriptChatAction> chat_actions;
  int64_t timestamp = 0;
  MessageDelivery delivery = MessageDelivery::Local;
  bool relay_visible = true;
};

struct RelayMessageBody {
  std::string text;
  std::optional<std::string> content_rml;
};

struct RelayEnvelope {
  std::string thread_id;
  std::string message_id;
  std::string sender_relay_id;
  RelayMessageBody body;
  int64_t timestamp = 0;
  std::string signature;
};

} // namespace pbr
