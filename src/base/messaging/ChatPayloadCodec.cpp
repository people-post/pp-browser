#include "base/messaging/ChatPayloadCodec.h"

#include "base/messaging/ChatPayloadTypes.h"
#include "base/messaging/MessagingLimits.h"

#include "common/Serialize.hpp"

#include <nlohmann/json.hpp>
#include <sstream>

namespace pbr {

namespace {

constexpr uint8_t kPayloadVersion = 1;
constexpr uint8_t kContentTypeText = 0;
constexpr uint8_t kContentTypeSystem = 1;
constexpr uint8_t kContentTypeAnnotation = 2;
constexpr uint8_t kContentTypeContactCard = 3;
constexpr uint8_t kContentTypeCryptoTx = 4;

constexpr uint8_t kSubVersion = 1;

void WriteLenUtf8(OutputArchive& ar, const std::string& value) {
  WireLenUtf8 field{value};
  ar & field;
}

Roe<std::vector<uint8_t>> FinalizePayload(std::ostringstream& oss) {
  const std::string data = oss.str();
  if (data.size() > kMaxE2ePlaintextBytes) {
    return Error("ChatPayload too large");
  }
  return std::vector<uint8_t>(data.begin(), data.end());
}

Roe<std::vector<uint8_t>> EncodePayloadBytes(const uint8_t content_type, const std::string& text,
                                             const std::function<void(OutputArchive&)>& write_tail) {
  std::ostringstream oss;
  OutputArchive ar(oss);
  ar & kPayloadVersion;
  ar & content_type;
  WriteLenUtf8(ar, text);
  if (write_tail) {
    write_tail(ar);
  }
  return FinalizePayload(oss);
}

std::string ReadLenUtf8(InputArchive& ar) {
  WireLenUtf8 field;
  ar & field;
  return field.value;
}

Roe<void> ReadExactEnd(InputArchive& ar) {
  if (ar.failed() || !ar.exactEnd()) {
    return Error("Malformed ChatPayload");
  }
  return {};
}

std::string PayloadJsonForMessage(const ThreadMessage& message) {
  nlohmann::json payload = nlohmann::json::object();
  if (message.content_type == ChatContentType::Annotation) {
    if (auto fields = ChatPayloadCodec::DecodeAnnotationJson(message.payload_json)) {
      payload = {{"annotation_type", fields->annotation_type},
                 {"target_message_id", fields->target_message_id},
                 {"value", fields->value}};
    }
  } else if (message.content_type == ChatContentType::ContactCard) {
    if (auto fields = ChatPayloadCodec::DecodeContactCardJson(message.payload_json)) {
      payload = {{"contact_id", fields->contact_id},
                 {"display_name", fields->display_name},
                 {"relay_user_id", fields->relay_user_id},
                 {"avatar_url", fields->avatar_url}};
    }
  } else if (message.content_type == ChatContentType::CryptoTx) {
    if (auto fields = ChatPayloadCodec::DecodeCryptoTxJson(message.payload_json)) {
      payload = {{"chain_id", fields->chain_id},
                 {"asset", fields->asset},
                 {"amount", fields->amount},
                 {"direction", fields->direction},
                 {"tx_hash", fields->tx_hash},
                 {"status", fields->status},
                 {"to_address", fields->to_address}};
    }
  } else {
    payload["text"] = message.text;
  }
  return payload.dump();
}

} // namespace

Roe<std::vector<uint8_t>> ChatPayloadCodec::EncodeText(const std::string& text) {
  return EncodePayloadBytes(kContentTypeText, text, nullptr);
}

Roe<std::vector<uint8_t>> ChatPayloadCodec::EncodeAnnotation(const ChatAnnotationFields& fields) {
  const std::string text = fields.text.empty() ? fields.value : fields.text;
  return EncodePayloadBytes(kContentTypeAnnotation, text, [&](OutputArchive& ar) {
    ar & kSubVersion;
    WriteLenUtf8(ar, fields.annotation_type);
    WriteLenUtf8(ar, fields.target_message_id);
    WriteLenUtf8(ar, fields.value);
  });
}

Roe<std::vector<uint8_t>> ChatPayloadCodec::EncodeContactCard(const ChatContactCardFields& fields,
                                                              const std::string& text) {
  return EncodePayloadBytes(kContentTypeContactCard, text, [&](OutputArchive& ar) {
    ar & kSubVersion;
    WriteLenUtf8(ar, fields.contact_id);
    WriteLenUtf8(ar, fields.display_name);
    WriteLenUtf8(ar, fields.relay_user_id);
    WriteLenUtf8(ar, fields.avatar_url);
  });
}

Roe<std::vector<uint8_t>> ChatPayloadCodec::EncodeCryptoTx(const ChatCryptoTxFields& fields, const std::string& text) {
  return EncodePayloadBytes(kContentTypeCryptoTx, text, [&](OutputArchive& ar) {
    ar & kSubVersion;
    WriteLenUtf8(ar, fields.chain_id);
    WriteLenUtf8(ar, fields.asset);
    WriteLenUtf8(ar, fields.amount);
    WriteLenUtf8(ar, fields.direction);
    WriteLenUtf8(ar, fields.tx_hash);
    WriteLenUtf8(ar, fields.status);
    WriteLenUtf8(ar, fields.to_address);
  });
}

Roe<ThreadMessage> ChatPayloadCodec::DecodeToMessageFields(const std::vector<uint8_t>& chat_payload) {
  ThreadMessage message;
  if (auto applied = ApplyRowToMessage(chat_payload, message)) {
    return message;
  }
  return Error("Failed to decode ChatPayload");
}

Roe<std::vector<uint8_t>> ChatPayloadCodec::EncodeToRow(const ThreadMessage& message) {
  switch (message.content_type) {
  case ChatContentType::System: {
    std::string control_type = "system";
    std::string detail;
    const nlohmann::json payload = nlohmann::json::parse(message.payload_json, nullptr, false);
    if (payload.is_object()) {
      if (payload.contains("control_type") && payload["control_type"].is_string()) {
        control_type = payload["control_type"].get<std::string>();
      }
      if (payload.contains("detail") && payload["detail"].is_string()) {
        detail = payload["detail"].get<std::string>();
      }
    }
    return EncodePayloadBytes(kContentTypeSystem, message.text, [&](OutputArchive& ar) {
      ar & kSubVersion;
      WriteLenUtf8(ar, control_type);
      WriteLenUtf8(ar, detail);
    });
  }
  case ChatContentType::Annotation: {
    ChatAnnotationFields fields;
    if (auto decoded = DecodeAnnotationJson(message.payload_json)) {
      fields = *decoded;
    }
    fields.text = message.text;
    if (message.target_message_id) {
      fields.target_message_id = *message.target_message_id;
    }
    return EncodeAnnotation(fields);
  }
  case ChatContentType::ContactCard: {
    ChatContactCardFields fields;
    if (auto decoded = DecodeContactCardJson(message.payload_json)) {
      fields = *decoded;
    }
    return EncodeContactCard(fields, message.text);
  }
  case ChatContentType::CryptoTx: {
    ChatCryptoTxFields fields;
    if (auto decoded = DecodeCryptoTxJson(message.payload_json)) {
      fields = *decoded;
    }
    return EncodeCryptoTx(fields, message.text);
  }
  case ChatContentType::Text:
  default:
    return EncodeText(message.text);
  }
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
  message.text = ReadLenUtf8(ar);
  if (ar.failed()) {
    return Error("Malformed ChatPayload");
  }

  if (content_type == kContentTypeText) {
    message.content_type = ChatContentType::Text;
    message.payload_json = nlohmann::json{{"text", message.text}}.dump();
    return ReadExactEnd(ar);
  }

  uint8_t sub_version = 0;
  ar & sub_version;
  if (ar.failed() || sub_version != kSubVersion) {
    return Error("Unsupported ChatPayload sub-version");
  }

  if (content_type == kContentTypeSystem) {
    message.content_type = ChatContentType::System;
    const std::string control_type = ReadLenUtf8(ar);
    const std::string detail = ReadLenUtf8(ar);
    message.payload_json = nlohmann::json{{"control_type", control_type}, {"detail", detail}}.dump();
    return ReadExactEnd(ar);
  }

  if (content_type == kContentTypeAnnotation) {
    message.content_type = ChatContentType::Annotation;
    ChatAnnotationFields fields;
    fields.text = message.text;
    fields.annotation_type = ReadLenUtf8(ar);
    fields.target_message_id = ReadLenUtf8(ar);
    fields.value = ReadLenUtf8(ar);
    message.target_message_id = fields.target_message_id;
    message.payload_json = nlohmann::json{{"annotation_type", fields.annotation_type},
                                          {"target_message_id", fields.target_message_id},
                                          {"value", fields.value}}
                               .dump();
    return ReadExactEnd(ar);
  }

  if (content_type == kContentTypeContactCard) {
    message.content_type = ChatContentType::ContactCard;
    ChatContactCardFields fields;
    fields.contact_id = ReadLenUtf8(ar);
    fields.display_name = ReadLenUtf8(ar);
    fields.relay_user_id = ReadLenUtf8(ar);
    fields.avatar_url = ReadLenUtf8(ar);
    message.payload_json = nlohmann::json{{"contact_id", fields.contact_id},
                                          {"display_name", fields.display_name},
                                          {"relay_user_id", fields.relay_user_id},
                                          {"avatar_url", fields.avatar_url}}
                               .dump();
    return ReadExactEnd(ar);
  }

  if (content_type == kContentTypeCryptoTx) {
    message.content_type = ChatContentType::CryptoTx;
    ChatCryptoTxFields fields;
    fields.chain_id = ReadLenUtf8(ar);
    fields.asset = ReadLenUtf8(ar);
    fields.amount = ReadLenUtf8(ar);
    fields.direction = ReadLenUtf8(ar);
    fields.tx_hash = ReadLenUtf8(ar);
    fields.status = ReadLenUtf8(ar);
    fields.to_address = ReadLenUtf8(ar);
    message.payload_json = nlohmann::json{{"chain_id", fields.chain_id},
                                          {"asset", fields.asset},
                                          {"amount", fields.amount},
                                          {"direction", fields.direction},
                                          {"tx_hash", fields.tx_hash},
                                          {"status", fields.status},
                                          {"to_address", fields.to_address}}
                               .dump();
    return ReadExactEnd(ar);
  }

  return Error("Unsupported ChatPayload content type");
}

Roe<ChatAnnotationFields> ChatPayloadCodec::DecodeAnnotationJson(const std::string& payload_json) {
  try {
    const nlohmann::json json = nlohmann::json::parse(payload_json);
    ChatAnnotationFields fields;
    fields.annotation_type = json.value("annotation_type", "");
    fields.target_message_id = json.value("target_message_id", "");
    fields.value = json.value("value", "");
    if (fields.annotation_type.empty() || fields.target_message_id.empty()) {
      return Error("Invalid annotation payload");
    }
    return fields;
  } catch (const std::exception&) {
    return Error("Invalid annotation JSON");
  }
}

Roe<ChatContactCardFields> ChatPayloadCodec::DecodeContactCardJson(const std::string& payload_json) {
  try {
    const nlohmann::json json = nlohmann::json::parse(payload_json);
    ChatContactCardFields fields;
    fields.contact_id = json.value("contact_id", "");
    fields.display_name = json.value("display_name", "");
    fields.relay_user_id = json.value("relay_user_id", "");
    fields.avatar_url = json.value("avatar_url", "");
    if (fields.contact_id.empty() || fields.display_name.empty()) {
      return Error("Invalid contact_card payload");
    }
    return fields;
  } catch (const std::exception&) {
    return Error("Invalid contact_card JSON");
  }
}

Roe<ChatCryptoTxFields> ChatPayloadCodec::DecodeCryptoTxJson(const std::string& payload_json) {
  try {
    const nlohmann::json json = nlohmann::json::parse(payload_json);
    ChatCryptoTxFields fields;
    fields.chain_id = json.value("chain_id", "");
    fields.asset = json.value("asset", "");
    fields.amount = json.value("amount", "");
    fields.direction = json.value("direction", "");
    fields.tx_hash = json.value("tx_hash", "");
    fields.status = json.value("status", "");
    fields.to_address = json.value("to_address", "");
    if (fields.chain_id.empty() || fields.asset.empty() || fields.amount.empty() || fields.direction.empty()) {
      return Error("Invalid crypto_tx payload");
    }
    return fields;
  } catch (const std::exception&) {
    return Error("Invalid crypto_tx JSON");
  }
}

std::string ChatPayloadCodec::BuildPayloadJson(const ThreadMessage& message) {
  return PayloadJsonForMessage(message);
}

} // namespace pbr
