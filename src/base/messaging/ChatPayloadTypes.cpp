#include "base/messaging/ChatPayloadTypes.h"

namespace pbr {

std::string ChatContentTypeToDb(const ChatContentType type) {
  switch (type) {
  case ChatContentType::System:
    return "system";
  case ChatContentType::Annotation:
    return "annotation";
  case ChatContentType::ContactCard:
    return "contact_card";
  case ChatContentType::CryptoTx:
    return "crypto_tx";
  case ChatContentType::Text:
  default:
    return "text";
  }
}

ChatContentType ChatContentTypeFromDb(const std::string& value) {
  if (value == "system") {
    return ChatContentType::System;
  }
  if (value == "annotation") {
    return ChatContentType::Annotation;
  }
  if (value == "contact_card") {
    return ChatContentType::ContactCard;
  }
  if (value == "crypto_tx") {
    return ChatContentType::CryptoTx;
  }
  return ChatContentType::Text;
}

} // namespace pbr
