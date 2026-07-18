#pragma once

#include "base/messaging/ChatActionTypes.h"
#include "base/messaging/ChatPayloadTypes.h"
#include "base/messaging/RelayEnvelope.h"
#include "base/messaging/ThreadChannel.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

inline constexpr const char* kLocalSelfContactId = "local:self";
inline constexpr const char* kAiAssistantContactId = "ai:assistant";

enum class ThreadKind { Ai, Direct, Group };

enum class MessageDelivery { Local, Pending, Relayed, Failed };

/** How a message reached this device (D051). */
enum class MessageTransport { Local, Relay, Direct };

struct DirectChatTarget {
  std::string peer_identity_kind;
  std::string peer_identity_value;
  ThreadChannel channel = ThreadChannel::E2e;

  bool operator==(const DirectChatTarget& other) const {
    return peer_identity_kind == other.peer_identity_kind &&
           peer_identity_value == other.peer_identity_value && channel == other.channel;
  }
};

struct Thread {
  std::string id;
  ThreadKind kind = ThreadKind::Ai;
  ThreadChannel channel = ThreadChannel::None;
  std::string title;
  /** Per-device group nickname override; wins over shared title at render time. */
  std::string local_title;
  std::vector<std::string> participant_contact_ids;
  std::string peer_identity_kind;
  std::string peer_identity_value;
  /** Wire group scope when kind=Group (D076). */
  std::optional<std::string> group_id;
  int64_t updated_at = 0;
  int unread_count = 0;
  std::string preview;
  bool encrypted = false;
};

struct ThreadMessage {
  std::string id;
  std::string thread_id;
  std::string sender_contact_id;
  int64_t display_order = 0;
  ChatContentType content_type = ChatContentType::Text;
  std::string text;
  /** Denormalized JSON tail for rich payload types (post-v4). */
  std::string payload_json;
  std::optional<std::string> target_message_id;
  std::optional<std::string> generation;
  std::optional<std::string> seq_owner_contact_id;
  std::optional<std::string> ai_invoke_mode;
  std::optional<std::string> content_rml;
  std::vector<TranscriptChatAction> chat_actions;
  int64_t timestamp = 0;
  MessageDelivery delivery = MessageDelivery::Local;
  bool relay_visible = true;
  std::optional<MessageTransport> transport;
  std::optional<uint64_t> sender_seq;
  std::optional<uint32_t> session_epoch;
};

struct ChatHistoryCursor {
  std::optional<uint64_t> next_min_sender_seq;
  std::optional<uint64_t> next_max_sender_seq;
};

struct ChatHistoryRequest {
  std::string requester_identity_kind;
  std::string requester_identity_value;
  std::string peer_identity_kind;
  std::string peer_identity_value;
  ThreadChannel channel = ThreadChannel::E2e;
  uint32_t session_epoch = 1;
  std::optional<uint64_t> min_sender_seq;
  std::optional<uint64_t> max_sender_seq;
  size_t limit = 50;
  std::string order = "asc";
};

struct ChatHistoryResponse {
  std::string peer_identity_kind;
  std::string peer_identity_value;
  ThreadChannel channel = ThreadChannel::E2e;
  uint32_t session_epoch = 1;
  std::vector<RelayEnvelope> messages;
  bool has_more = false;
  ChatHistoryCursor cursor;
};

} // namespace pbr
