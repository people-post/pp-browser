#include "base/messaging/TranscriptBodyCodec.h"

#include "base/crypto/CryptoUtil.h"
#include "base/messaging/ChatPayloadCodec.h"

#include <nlohmann/json.hpp>

namespace pbr {

namespace {

constexpr uint8_t kBodyVersion = 1;

nlohmann::json ChatActionsToJson(const std::vector<TranscriptChatAction>& actions) {
  nlohmann::json out = nlohmann::json::array();
  for (const TranscriptChatAction& action : actions) {
    nlohmann::json item = {{"label", action.label}, {"message", action.message}};
    if (action.payload) {
      item["payload"] = *action.payload;
    }
    out.push_back(std::move(item));
  }
  return out;
}

std::vector<TranscriptChatAction> ChatActionsFromJson(const nlohmann::json& parsed) {
  std::vector<TranscriptChatAction> out;
  if (!parsed.is_array()) {
    return out;
  }
  for (const auto& item : parsed) {
    TranscriptChatAction action;
    if (item.contains("label") && item["label"].is_string()) {
      action.label = item["label"].get<std::string>();
    }
    if (item.contains("message") && item["message"].is_string()) {
      action.message = item["message"].get<std::string>();
    }
    if (item.contains("payload") && item["payload"].is_string()) {
      action.payload = item["payload"].get<std::string>();
    }
    out.push_back(std::move(action));
  }
  return out;
}

} // namespace

Roe<std::vector<uint8_t>> TranscriptBodyCodec::Encode(const TranscriptBodyPlaintext& body) {
  nlohmann::json json;
  json["v"] = kBodyVersion;
  json["chat_payload_b64"] = Base64Encode(body.chat_payload);
  json["text"] = body.text;
  json["payload"] = body.payload_json;
  if (body.content_rml) {
    json["content_rml"] = *body.content_rml;
  }
  json["chat_actions"] = ChatActionsToJson(body.chat_actions);
  const std::string serialized = json.dump();
  return std::vector<uint8_t>(serialized.begin(), serialized.end());
}

Roe<TranscriptBodyPlaintext> TranscriptBodyCodec::Decode(const std::vector<uint8_t>& bytes) {
  const std::string serialized(bytes.begin(), bytes.end());
  const nlohmann::json json = nlohmann::json::parse(serialized, nullptr, false);
  if (json.is_discarded() || !json.is_object()) {
    return Error("Invalid transcript body JSON");
  }
  if (json.value("v", 0) != kBodyVersion) {
    return Error("Unsupported transcript body version");
  }
  if (!json.contains("chat_payload_b64") || !json["chat_payload_b64"].is_string()) {
    return Error("Missing transcript chat_payload");
  }
  auto chat_payload = Base64Decode(json["chat_payload_b64"].get<std::string>());
  if (!chat_payload) {
    return chat_payload.error();
  }
  TranscriptBodyPlaintext body;
  body.chat_payload = std::move(*chat_payload);
  body.text = json.value("text", std::string{});
  body.payload_json = json.value("payload", std::string{});
  if (json.contains("content_rml") && json["content_rml"].is_string()) {
    body.content_rml = json["content_rml"].get<std::string>();
  }
  if (json.contains("chat_actions")) {
    body.chat_actions = ChatActionsFromJson(json["chat_actions"]);
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
