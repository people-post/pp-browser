#include "base/messaging/ChatPayloadValidator.h"

#include "base/messaging/ChatPayloadCodec.h"
#include "base/messaging/ChatPayloadTypes.h"
#include "base/messaging/MessagingLimits.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

bool IsSupportedContentType(const ChatContentType type) {
  switch (type) {
  case ChatContentType::Text:
  case ChatContentType::System:
  case ChatContentType::Annotation:
  case ChatContentType::ContactCard:
  case ChatContentType::CryptoTx:
  case ChatContentType::Attachment:
  case ChatContentType::Unsupported:
    return true;
  }
  return false;
}

} // namespace

Roe<ThreadMessage> ChatPayloadValidator::DecodeValidated(const std::vector<uint8_t>& chat_payload) {
  if (chat_payload.size() > kMaxE2ePlaintextBytes) {
    return Error("ChatPayload too large");
  }
  ThreadMessage message;
  if (auto applied = ChatPayloadCodec::ApplyRowToMessage(chat_payload, message)) {
    if (!IsSupportedContentType(message.content_type)) {
      return Error("Unsupported ChatPayload content type");
    }
    if (message.content_type == ChatContentType::Annotation) {
      if (!message.target_message_id || message.target_message_id->empty()) {
        return Error("Annotation missing target_message_id");
      }
      if (auto fields = ChatPayloadCodec::DecodeAnnotationJson(message.payload_json); !fields) {
        return fields.error();
      }
    } else if (message.content_type == ChatContentType::ContactCard) {
      if (auto fields = ChatPayloadCodec::DecodeContactCardJson(message.payload_json); !fields) {
        return fields.error();
      }
    } else if (message.content_type == ChatContentType::CryptoTx) {
      if (auto fields = ChatPayloadCodec::DecodeCryptoTxJson(message.payload_json); !fields) {
        return fields.error();
      }
    } else if (message.content_type == ChatContentType::Attachment) {
      if (auto fields = ChatPayloadCodec::DecodeAttachmentJson(message.payload_json); !fields) {
        return fields.error();
      }
    }
    SanitizeInboundFields(message);
    return message;
  }
  return Error("Invalid ChatPayload");
}

Roe<void> ChatPayloadValidator::ValidateOutboundText(const std::string& text) {
  if (text.size() > kMaxComposeTextBytes) {
    return Error("Message text exceeds compose limit");
  }
  return {};
}

void ChatPayloadValidator::SanitizeInboundFields(ThreadMessage& message) {
  message.content_rml.reset();
  message.chat_actions.clear();
}

} // namespace pbr
