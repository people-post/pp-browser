#include "base/messaging/RelayWirePayload.h"

#include "base/crypto/CryptoUtil.h"
#include "base/messaging/ChatPayloadCodec.h"

namespace pbr {

Roe<std::string> RelayWirePayload::EncodePlaintextText(const std::string& text) {
  auto bytes = ChatPayloadCodec::EncodeText(text);
  if (!bytes) {
    return bytes.error();
  }
  return Base64Encode(*bytes);
}

Roe<std::string> RelayWirePayload::DecodePlaintextText(const std::string& payload_b64) {
  auto bytes = Base64Decode(payload_b64);
  if (!bytes) {
    return bytes.error();
  }
  auto fields = ChatPayloadCodec::DecodeToMessageFields(*bytes);
  if (!fields) {
    return fields.error();
  }
  if (fields->content_type != ChatContentType::Text) {
    return Error("Unsupported relay payload content type");
  }
  return fields->text;
}

} // namespace pbr
