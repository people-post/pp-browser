#include "domain/messaging/TranscriptBodyCodec.h"

#include "foundation/crypto/CryptoUtil.h"
#include "domain/messaging/ChatPayloadCodec.h"
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr uint8_t kBodyVersion = 1;

Value ChatActionsToJson(const std::vector<TranscriptChatAction>& actions) {
  std::vector<Value> out;
  out.reserve(actions.size());
  for (const TranscriptChatAction& action : actions) {
    Object item;
    item.set("label", action.label);
    item.set("message", action.message);
    if (action.payload) {
      item.set("payload", *action.payload);
    }
    out.push_back(ObjectValue(std::move(item)));
  }
  return ArrayValue(std::move(out));
}

std::vector<TranscriptChatAction> ChatActionsFromJson(const Array* arr) {
  std::vector<TranscriptChatAction> out;
  if (!arr) {
    return out;
  }
  for (const Value& item_value : arr->elements) {
    const Object* item = asObject(item_value);
    if (!item) {
      continue;
    }
    TranscriptChatAction action;
    if (auto label = item->getString("label")) {
      action.label = *label;
    }
    if (auto message = item->getString("message")) {
      action.message = *message;
    }
    if (auto payload = item->getString("payload")) {
      action.payload = *payload;
    }
    out.push_back(std::move(action));
  }
  return out;
}

} // namespace

Roe<std::vector<uint8_t>> TranscriptBodyCodec::Encode(const TranscriptBodyPlaintext& body) {
  Object json;
  json.set("v", static_cast<int64_t>(kBodyVersion));
  json.set("chat_payload_b64", Base64Encode(body.chat_payload));
  json.set("text", body.text);
  json.set("payload", body.payload_json);
  if (body.content_rml) {
    json.set("content_rml", *body.content_rml);
  }
  json.set("chat_actions", ChatActionsToJson(body.chat_actions));
  const std::string serialized = DumpJson(json);
  return std::vector<uint8_t>(serialized.begin(), serialized.end());
}

Roe<TranscriptBodyPlaintext> TranscriptBodyCodec::Decode(const std::vector<uint8_t>& bytes) {
  const std::string serialized(bytes.begin(), bytes.end());
  auto json = TryParseObject(serialized);
  if (!json) {
    return Error("Invalid transcript body JSON");
  }
  if (json->getIf<int64_t>("v").value_or(0) != kBodyVersion) {
    return Error("Unsupported transcript body version");
  }
  auto chat_payload_b64 = json->getString("chat_payload_b64");
  if (!chat_payload_b64) {
    return Error("Missing transcript chat_payload");
  }
  auto chat_payload = Base64Decode(*chat_payload_b64);
  if (!chat_payload) {
    return chat_payload.error();
  }
  TranscriptBodyPlaintext body;
  body.chat_payload = std::move(*chat_payload);
  body.text = json->getString("text").value_or(std::string{});
  body.payload_json = json->getString("payload").value_or(std::string{});
  if (auto content_rml = json->getString("content_rml")) {
    body.content_rml = *content_rml;
  }
  if (const Array* chat_actions = json->getArray("chat_actions")) {
    body.chat_actions = ChatActionsFromJson(chat_actions);
  }
  return body;
}

Roe<TranscriptBodyPlaintext> TranscriptBodyCodec::FromMessage(const ThreadMessage& message) {
  auto chat_payload = ChatPayloadCodec::EncodeToRow(message);
  if (!chat_payload) {
    return chat_payload.error();
  }
  TranscriptBodyPlaintext body;
  body.chat_payload = std::move(*chat_payload);
  body.text = message.text;
  body.payload_json =
      message.payload_json.empty() ? ChatPayloadCodec::BuildPayloadJson(message) : message.payload_json;
  body.content_rml = message.content_rml;
  body.chat_actions = message.chat_actions;
  return body;
}

Roe<void> TranscriptBodyCodec::ApplyToMessage(const TranscriptBodyPlaintext& body, ThreadMessage& message) {
  message.text = body.text;
  message.payload_json = body.payload_json;
  message.content_rml = body.content_rml;
  message.chat_actions = body.chat_actions;
  if (auto applied = ChatPayloadCodec::ApplyRowToMessage(body.chat_payload, message); !applied) {
    return applied.error();
  }
  return {};
}

} // namespace pbr
