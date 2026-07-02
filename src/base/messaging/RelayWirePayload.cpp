#include "base/messaging/RelayWirePayload.h"

#include "base/crypto/CryptoUtil.h"
#include "base/messaging/ChatPayloadCodec.h"
#include "base/messaging/ChatPayloadValidator.h"

namespace pbr {

Roe<std::string> RelayWirePayload::EncodePlaintextText(const std::string& text) {
  if (auto valid = ChatPayloadValidator::ValidateOutboundText(text); !valid) {
    return valid.error();
  }
  auto bytes = ChatPayloadCodec::EncodeText(text);
  if (!bytes) {
    return bytes.error();
  }
  return Base64Encode(*bytes);
}

Roe<ThreadMessage> RelayWirePayload::DecodeInboundPayload(const std::string& payload_b64) {
  auto bytes = Base64Decode(payload_b64);
  if (!bytes) {
    return bytes.error();
  }
  return ChatPayloadValidator::DecodeValidated(*bytes);
}

Roe<std::string> RelayWirePayload::DecodePlaintextText(const std::string& payload_b64) {
  auto message = DecodeInboundPayload(payload_b64);
  if (!message) {
    return message.error();
  }
  if (message->content_type != ChatContentType::Text) {
    return Error("Unsupported relay payload content type");
  }
  return message->text;
}

} // namespace pbr
