#include "base/messaging/ChatPayloadValidator.h"

#include "base/messaging/ChatPayloadCodec.h"
#include "base/messaging/MessagingLimits.h"

namespace pbr {

Roe<ThreadMessage> ChatPayloadValidator::DecodeValidated(const std::vector<uint8_t>& chat_payload) {
  if (chat_payload.size() > kMaxE2ePlaintextBytes) {
    return Error("ChatPayload too large");
  }
  ThreadMessage message;
  if (auto applied = ChatPayloadCodec::ApplyRowToMessage(chat_payload, message)) {
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
