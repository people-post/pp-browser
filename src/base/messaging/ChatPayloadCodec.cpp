#include "base/messaging/ChatPayloadCodec.h"

#include "base/messaging/MessagingLimits.h"

#include "common/Serialize.hpp"

#include <sstream>

namespace pbr {

namespace {

constexpr uint8_t kPayloadVersion = 1;
constexpr uint8_t kContentTypeText = 0;
constexpr uint8_t kContentTypeSystem = 1;

Roe<std::vector<uint8_t>> EncodePayloadBytes(const uint8_t content_type, const std::string& text,
                                             const std::optional<std::string>& control_type = std::nullopt,
                                             const std::optional<std::string>& detail = std::nullopt) {
  std::ostringstream oss;
  OutputArchive ar(oss);
  ar & kPayloadVersion;
  ar & content_type;
  WireLenUtf8 text_field{text};
  ar & text_field;
  if (content_type == kContentTypeSystem) {
    const uint8_t sub_version = 1;
    ar & sub_version;
    WireLenUtf8 control{control_type.value_or("")};
    ar & control;
    WireLenUtf8 detail_field{detail.value_or("")};
    ar & detail_field;
  }
  const std::string data = oss.str();
  if (data.size() > kMaxE2ePlaintextBytes) {
    return Error("ChatPayload too large");
  }
  return std::vector<uint8_t>(data.begin(), data.end());
}

} // namespace

Roe<std::vector<uint8_t>> ChatPayloadCodec::EncodeText(const std::string& text) {
  return EncodePayloadBytes(kContentTypeText, text);
}

Roe<ThreadMessage> ChatPayloadCodec::DecodeToMessageFields(const std::vector<uint8_t>& chat_payload) {
  ThreadMessage message;
  if (auto applied = ApplyRowToMessage(chat_payload, message)) {
    return message;
  }
  return Error("Failed to decode ChatPayload");
}

Roe<std::vector<uint8_t>> ChatPayloadCodec::EncodeToRow(const ThreadMessage& message) {
  if (message.content_type == ChatContentType::System) {
    return EncodePayloadBytes(kContentTypeSystem, message.text, "system", "");
  }
  return EncodePayloadBytes(kContentTypeText, message.text);
}

Roe<void> ChatPayloadCodec::ApplyRowToMessage(const std::vector<uint8_t>& chat_payload, ThreadMessage& message) {
  if (chat_payload.size() > kMaxE2ePlaintextBytes) {
    return Error("ChatPayload too large");
  }
  const std::string data(chat_payload.begin(), chat_payload.end());
  std::istringstream iss(data);
  InputArchive ar(iss);

  uint8_t payload_version = 0;
  ar & payload_version;
  if (ar.failed() || payload_version != kPayloadVersion) {
    return Error("Unsupported ChatPayload version");
  }
  uint8_t content_type = 0;
  ar & content_type;
  WireLenUtf8 text_field;
  ar & text_field;
  message.text = text_field.value;
  if (content_type == kContentTypeText) {
    message.content_type = ChatContentType::Text;
  } else if (content_type == kContentTypeSystem) {
    message.content_type = ChatContentType::System;
    uint8_t sub_version = 0;
    ar & sub_version;
    WireLenUtf8 control;
    ar & control;
    WireLenUtf8 detail;
    ar & detail;
    (void)sub_version;
    (void)control;
    (void)detail;
  } else {
    return Error("Unsupported ChatPayload content type");
  }
  if (ar.failed() || !ar.exactEnd()) {
    return Error("Malformed ChatPayload");
  }
  return {};
}

} // namespace pbr
