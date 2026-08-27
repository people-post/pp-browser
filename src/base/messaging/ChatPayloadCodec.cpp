#include "base/messaging/ChatPayloadCodec.h"

#include "base/crypto/AttachmentContentHash.h"
#include "base/messaging/ChatPayloadTypes.h"
#include "base/messaging/MessagingLimits.h"
#include "common/Serialize.hpp"
#include "common/ValueJson.h"

#include <functional>
#include <sstream>

namespace pbr {

namespace attachment_codec {

Value BytesToJsonArray(const std::vector<uint8_t>& bytes) {
  std::vector<Value> out;
  out.reserve(bytes.size());
  for (const uint8_t byte : bytes) {
    out.push_back(Value(static_cast<int64_t>(byte)));
  }
  return ArrayValue(std::move(out));
}

Object AttachmentFieldsJsonObject(const ChatAttachmentFields& fields) {
  Object out;
  out.set("url", fields.url);
  out.set("mime", fields.mime);
  out.set("filename", fields.filename);
  out.setJsonUInt("byte_length", fields.byte_length);
  out.set("content_hash", BytesToJsonArray(fields.content_hash));
  out.set("content_key", BytesToJsonArray(fields.content_key));
  out.set("blob_nonce", BytesToJsonArray(fields.blob_nonce));
  return out;
}

std::vector<uint8_t> ReadByteArray(const Object& json, const char* key) {
  std::vector<uint8_t> out;
  const Array* arr = json.getArray(key);
  if (!arr) {
    return out;
  }
  for (const Value& byte : arr->elements) {
    if (auto v = asNonNegInt(byte)) {
      out.push_back(static_cast<uint8_t>(*v));
    }
  }
  return out;
}

} // namespace attachment_codec

namespace {

constexpr uint8_t kPayloadVersion = 1;
constexpr uint8_t kContentTypeText = 0;
constexpr uint8_t kContentTypeSystem = 1;
constexpr uint8_t kContentTypeAnnotation = 2;
constexpr uint8_t kContentTypeContactCard = 3;
constexpr uint8_t kContentTypeCryptoTx = 4;
constexpr uint8_t kContentTypeAttachment = 5;

constexpr uint8_t kSubVersion = 1;

void WriteLenUtf8(OutputArchive& ar, const std::string& value) {
  WireLenUtf8 field{value};
  ar & field;
}

void WriteLenBytes(OutputArchive& ar, const std::vector<uint8_t>& value) {
  WireLenBytes field{value};
  ar & field;
}

std::vector<uint8_t> ReadLenBytes(InputArchive& ar) {
  WireLenBytes field;
  ar & field;
  return field.value;
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
  Object payload;
  if (message.content_type == ChatContentType::Annotation) {
    if (auto fields = ChatPayloadCodec::DecodeAnnotationJson(message.payload_json)) {
      payload.set("annotation_type", fields->annotation_type);
      payload.set("target_message_id", fields->target_message_id);
      payload.set("value", fields->value);
    }
  } else if (message.content_type == ChatContentType::ContactCard) {
    if (auto fields = ChatPayloadCodec::DecodeContactCardJson(message.payload_json)) {
      payload.set("contact_id", fields->contact_id);
      payload.set("display_name", fields->display_name);
      payload.set("relay_user_id", fields->relay_user_id);
      payload.set("avatar_url", fields->avatar_url);
    }
  } else if (message.content_type == ChatContentType::CryptoTx) {
    if (auto fields = ChatPayloadCodec::DecodeCryptoTxJson(message.payload_json)) {
      payload.set("chain_id", fields->chain_id);
      payload.set("asset", fields->asset);
      payload.set("amount", fields->amount);
      payload.set("direction", fields->direction);
      payload.set("tx_hash", fields->tx_hash);
      payload.set("status", fields->status);
      payload.set("to_address", fields->to_address);
    }
  } else if (message.content_type == ChatContentType::Attachment) {
    if (auto fields = ChatPayloadCodec::DecodeAttachmentJson(message.payload_json)) {
      payload = attachment_codec::AttachmentFieldsJsonObject(*fields);
    }
  } else if (message.content_type == ChatContentType::Unsupported) {
    if (auto parsed = TryParseObject(message.payload_json)) {
      payload = std::move(*parsed);
    }
  } else {
    payload.set("text", message.text);
  }
  return DumpJson(payload);
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

Roe<std::vector<uint8_t>> ChatPayloadCodec::EncodeAttachment(const ChatAttachmentFields& fields,
                                                             const std::string& text) {
  return EncodePayloadBytes(kContentTypeAttachment, text, [&](OutputArchive& ar) {
    ar & kSubVersion;
    WriteLenUtf8(ar, fields.url);
    WriteLenUtf8(ar, fields.mime);
    WriteLenUtf8(ar, fields.filename);
    ar & fields.byte_length;
    WriteLenBytes(ar, fields.content_hash);
    WriteLenBytes(ar, fields.content_key);
    WriteLenBytes(ar, fields.blob_nonce);
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
    if (auto payload = TryParseObject(message.payload_json)) {
      if (auto ct = payload->getString("control_type")) {
        control_type = *ct;
      }
      if (auto d = payload->getString("detail")) {
        detail = *d;
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
  case ChatContentType::Attachment: {
    ChatAttachmentFields fields;
    if (auto decoded = DecodeAttachmentJson(message.payload_json)) {
      fields = *decoded;
    }
    return EncodeAttachment(fields, message.text);
  }
  case ChatContentType::Unsupported:
    return Error("Cannot encode unsupported ChatPayload");
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
    Object payload;
    payload.set("text", message.text);
    message.payload_json = DumpJson(payload);
    return ReadExactEnd(ar);
  }

  if (content_type != kContentTypeSystem && content_type != kContentTypeAnnotation &&
      content_type != kContentTypeContactCard && content_type != kContentTypeCryptoTx &&
      content_type != kContentTypeAttachment) {
    message.content_type = ChatContentType::Unsupported;
    Object payload;
    payload.set("wire_content_type", static_cast<int64_t>(content_type));
    message.payload_json = DumpJson(payload);
    return {};
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
    Object payload;
    payload.set("control_type", control_type);
    payload.set("detail", detail);
    message.payload_json = DumpJson(payload);
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
    Object payload;
    payload.set("annotation_type", fields.annotation_type);
    payload.set("target_message_id", fields.target_message_id);
    payload.set("value", fields.value);
    message.payload_json = DumpJson(payload);
    return ReadExactEnd(ar);
  }

  if (content_type == kContentTypeContactCard) {
    message.content_type = ChatContentType::ContactCard;
    ChatContactCardFields fields;
    fields.contact_id = ReadLenUtf8(ar);
    fields.display_name = ReadLenUtf8(ar);
    fields.relay_user_id = ReadLenUtf8(ar);
    fields.avatar_url = ReadLenUtf8(ar);
    Object payload;
    payload.set("contact_id", fields.contact_id);
    payload.set("display_name", fields.display_name);
    payload.set("relay_user_id", fields.relay_user_id);
    payload.set("avatar_url", fields.avatar_url);
    message.payload_json = DumpJson(payload);
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
    Object payload;
    payload.set("chain_id", fields.chain_id);
    payload.set("asset", fields.asset);
    payload.set("amount", fields.amount);
    payload.set("direction", fields.direction);
    payload.set("tx_hash", fields.tx_hash);
    payload.set("status", fields.status);
    payload.set("to_address", fields.to_address);
    message.payload_json = DumpJson(payload);
    return ReadExactEnd(ar);
  }

  if (content_type == kContentTypeAttachment) {
    message.content_type = ChatContentType::Attachment;
    ChatAttachmentFields fields;
    fields.url = ReadLenUtf8(ar);
    fields.mime = ReadLenUtf8(ar);
    fields.filename = ReadLenUtf8(ar);
    ar & fields.byte_length;
    fields.content_hash = ReadLenBytes(ar);
    fields.content_key = ReadLenBytes(ar);
    fields.blob_nonce = ReadLenBytes(ar);
    if (ar.failed()) {
      return Error("Malformed attachment ChatPayload");
    }
    message.payload_json = DumpJson(attachment_codec::AttachmentFieldsJsonObject(fields));
    return ReadExactEnd(ar);
  }

  return Error("Unsupported ChatPayload content type");
}

Roe<ChatAnnotationFields> ChatPayloadCodec::DecodeAnnotationJson(const std::string& payload_json) {
  auto json = TryParseObject(payload_json);
  if (!json) {
    return Error("Invalid annotation JSON");
  }
  ChatAnnotationFields fields;
  fields.annotation_type = json->getString("annotation_type").value_or("");
  fields.target_message_id = json->getString("target_message_id").value_or("");
  fields.value = json->getString("value").value_or("");
  if (fields.annotation_type.empty() || fields.target_message_id.empty()) {
    return Error("Invalid annotation payload");
  }
  return fields;
}

Roe<ChatContactCardFields> ChatPayloadCodec::DecodeContactCardJson(const std::string& payload_json) {
  auto json = TryParseObject(payload_json);
  if (!json) {
    return Error("Invalid contact_card JSON");
  }
  ChatContactCardFields fields;
  fields.contact_id = json->getString("contact_id").value_or("");
  fields.display_name = json->getString("display_name").value_or("");
  fields.relay_user_id = json->getString("relay_user_id").value_or("");
  fields.avatar_url = json->getString("avatar_url").value_or("");
  if (fields.contact_id.empty() || fields.display_name.empty()) {
    return Error("Invalid contact_card payload");
  }
  return fields;
}

Roe<ChatCryptoTxFields> ChatPayloadCodec::DecodeCryptoTxJson(const std::string& payload_json) {
  auto json = TryParseObject(payload_json);
  if (!json) {
    return Error("Invalid crypto_tx JSON");
  }
  ChatCryptoTxFields fields;
  fields.chain_id = json->getString("chain_id").value_or("");
  fields.asset = json->getString("asset").value_or("");
  fields.amount = json->getString("amount").value_or("");
  fields.direction = json->getString("direction").value_or("");
  fields.tx_hash = json->getString("tx_hash").value_or("");
  fields.status = json->getString("status").value_or("");
  fields.to_address = json->getString("to_address").value_or("");
  if (fields.chain_id.empty() || fields.asset.empty() || fields.amount.empty() || fields.direction.empty()) {
    return Error("Invalid crypto_tx payload");
  }
  return fields;
}

constexpr size_t kAttachmentContentKeySize = 32;
constexpr size_t kAttachmentBlobNonceSize = 24;

Roe<ChatAttachmentFields> ChatPayloadCodec::DecodeAttachmentJson(const std::string& payload_json) {
  auto json = TryParseObject(payload_json);
  if (!json) {
    return Error("Invalid attachment JSON");
  }
  ChatAttachmentFields fields;
  fields.url = json->getString("url").value_or("");
  fields.mime = json->getString("mime").value_or("");
  fields.filename = json->getString("filename").value_or("");
  fields.byte_length = json->getNonNegInt("byte_length").value_or(0);
  fields.content_hash = attachment_codec::ReadByteArray(*json, "content_hash");
  fields.content_key = attachment_codec::ReadByteArray(*json, "content_key");
  fields.blob_nonce = attachment_codec::ReadByteArray(*json, "blob_nonce");
  if (fields.url.empty() || fields.mime.empty() || fields.byte_length == 0 ||
      fields.content_hash.size() != kAttachmentContentHashSize ||
      fields.content_key.size() != kAttachmentContentKeySize ||
      fields.blob_nonce.size() != kAttachmentBlobNonceSize) {
    return Error("Invalid attachment payload");
  }
  return fields;
}

std::string ChatPayloadCodec::AttachmentFieldsToJson(const ChatAttachmentFields& fields) {
  return DumpJson(attachment_codec::AttachmentFieldsJsonObject(fields));
}

std::string ChatPayloadCodec::BuildPayloadJson(const ThreadMessage& message) {
  return PayloadJsonForMessage(message);
}

} // namespace pbr
