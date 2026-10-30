#pragma once

#include "common/chat/ChatPayloadTypes.h"
#include "common/thread/ThreadTypes.h"

#include "common/Error.h"
#include "lib/messaging/GroupMentions.h"

#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Binary ChatPayload v1 codec (D087) — text, system, and post-v4 rich types. */
class ChatPayloadCodec {
public:
  static Roe<std::vector<uint8_t>> EncodeText(const std::string& text);
  /** Text message carrying @mentions (content type 6). Ranges that fail the content check are dropped. */
  static Roe<std::vector<uint8_t>> EncodeTextMentions(const std::string& text, const MentionsPayload& mentions);
  /** True for a Text message whose payload_json carries @mentions: it must go through EncodeToRow, not EncodeText. */
  static bool HasMentions(const ThreadMessage& message);
  static Roe<std::vector<uint8_t>> EncodeAnnotation(const ChatAnnotationFields& fields);
  static Roe<std::vector<uint8_t>> EncodeContactCard(const ChatContactCardFields& fields, const std::string& text);
  static Roe<std::vector<uint8_t>> EncodeCryptoTx(const ChatCryptoTxFields& fields, const std::string& text);
  static Roe<std::vector<uint8_t>> EncodeAttachment(const ChatAttachmentFields& fields, const std::string& text);

  static Roe<ThreadMessage> DecodeToMessageFields(const std::vector<uint8_t>& chat_payload);
  static Roe<std::vector<uint8_t>> EncodeToRow(const ThreadMessage& message);
  static Roe<void> ApplyRowToMessage(const std::vector<uint8_t>& chat_payload, ThreadMessage& message);

  static Roe<ChatAnnotationFields> DecodeAnnotationJson(const std::string& payload_json);
  static Roe<ChatContactCardFields> DecodeContactCardJson(const std::string& payload_json);
  static Roe<ChatCryptoTxFields> DecodeCryptoTxJson(const std::string& payload_json);
  static Roe<ChatAttachmentFields> DecodeAttachmentJson(const std::string& payload_json);
  static std::string AttachmentFieldsToJson(const ChatAttachmentFields& fields);
  static std::string ContactCardFieldsToJson(const ChatContactCardFields& fields);
  static std::string BuildPayloadJson(const ThreadMessage& message);
};

} // namespace pbr
