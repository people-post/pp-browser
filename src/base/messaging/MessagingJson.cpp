#include "base/messaging/MessagingJson.h"

#include <nlohmann/json.hpp>

namespace pbr {

namespace {

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

std::vector<TranscriptChatAction> ChatActionsFromJson(const nlohmann::json& json) {
  std::vector<TranscriptChatAction> out;
  if (!json.is_array()) {
    return out;
  }
  for (const auto& item : json) {
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

std::string ThreadKindToString(const ThreadKind kind) {
  switch (kind) {
  case ThreadKind::Ai:
    return "ai";
  case ThreadKind::Direct:
    return "direct";
  case ThreadKind::Group:
    return "group";
  }
  return "ai";
}

ThreadKind ThreadKindFromString(const std::string& value) {
  if (value == "direct") {
    return ThreadKind::Direct;
  }
  if (value == "group") {
    return ThreadKind::Group;
  }
  return ThreadKind::Ai;
}

std::string MessageDeliveryToString(const MessageDelivery delivery) {
  switch (delivery) {
  case MessageDelivery::Local:
    return "local";
  case MessageDelivery::Pending:
    return "pending";
  case MessageDelivery::Relayed:
    return "relayed";
  case MessageDelivery::Failed:
    return "failed";
  }
  return "local";
}

MessageDelivery MessageDeliveryFromString(const std::string& value) {
  if (value == "pending") {
    return MessageDelivery::Pending;
  }
  if (value == "relayed") {
    return MessageDelivery::Relayed;
  }
  if (value == "failed") {
    return MessageDelivery::Failed;
  }
  return MessageDelivery::Local;
}

nlohmann::json ThreadToJson(const Thread& thread) {
  return {{"id", thread.id},
          {"kind", ThreadKindToString(thread.kind)},
          {"title", thread.title},
          {"participant_contact_ids", thread.participant_contact_ids},
          {"updated_at", thread.updated_at},
          {"unread_count", thread.unread_count},
          {"preview", thread.preview}};
}

Thread ThreadFromJson(const nlohmann::json& json) {
  Thread thread;
  if (json.contains("id") && json["id"].is_string()) {
    thread.id = json["id"].get<std::string>();
  }
  if (json.contains("kind") && json["kind"].is_string()) {
    thread.kind = ThreadKindFromString(json["kind"].get<std::string>());
  }
  if (json.contains("title") && json["title"].is_string()) {
    thread.title = json["title"].get<std::string>();
  }
  if (json.contains("participant_contact_ids") && json["participant_contact_ids"].is_array()) {
    for (const auto& id : json["participant_contact_ids"]) {
      if (id.is_string()) {
        thread.participant_contact_ids.push_back(id.get<std::string>());
      }
    }
  }
  if (json.contains("updated_at") && json["updated_at"].is_number_integer()) {
    thread.updated_at = json["updated_at"].get<int64_t>();
  }
  if (json.contains("unread_count") && json["unread_count"].is_number_integer()) {
    thread.unread_count = json["unread_count"].get<int>();
  }
  if (json.contains("preview") && json["preview"].is_string()) {
    thread.preview = json["preview"].get<std::string>();
  }
  return thread;
}

nlohmann::json ThreadMessageToJson(const ThreadMessage& message) {
  nlohmann::json json = {{"id", message.id},
                         {"thread_id", message.thread_id},
                         {"sender_contact_id", message.sender_contact_id},
                         {"text", message.text},
                         {"timestamp", message.timestamp},
                         {"delivery", MessageDeliveryToString(message.delivery)},
                         {"relay_visible", message.relay_visible},
                         {"chat_actions", ChatActionsToJson(message.chat_actions)}};
  if (message.content_rml) {
    json["content_rml"] = *message.content_rml;
  }
  return json;
}

ThreadMessage ThreadMessageFromJson(const nlohmann::json& json) {
  ThreadMessage message;
  if (json.contains("id") && json["id"].is_string()) {
    message.id = json["id"].get<std::string>();
  }
  if (json.contains("thread_id") && json["thread_id"].is_string()) {
    message.thread_id = json["thread_id"].get<std::string>();
  }
  if (json.contains("sender_contact_id") && json["sender_contact_id"].is_string()) {
    message.sender_contact_id = json["sender_contact_id"].get<std::string>();
  }
  if (json.contains("text") && json["text"].is_string()) {
    message.text = json["text"].get<std::string>();
  }
  if (json.contains("content_rml") && json["content_rml"].is_string()) {
    message.content_rml = json["content_rml"].get<std::string>();
  }
  if (json.contains("timestamp") && json["timestamp"].is_number_integer()) {
    message.timestamp = json["timestamp"].get<int64_t>();
  }
  if (json.contains("delivery") && json["delivery"].is_string()) {
    message.delivery = MessageDeliveryFromString(json["delivery"].get<std::string>());
  }
  if (json.contains("relay_visible") && json["relay_visible"].is_boolean()) {
    message.relay_visible = json["relay_visible"].get<bool>();
  }
  if (json.contains("chat_actions")) {
    message.chat_actions = ChatActionsFromJson(json["chat_actions"]);
  }
  return message;
}

nlohmann::json RelayEnvelopeToJson(const RelayEnvelope& envelope) {
  nlohmann::json body = {{"text", envelope.body.text}};
  if (envelope.body.content_rml) {
    body["content_rml"] = *envelope.body.content_rml;
  }
  return {{"thread_id", envelope.thread_id},
          {"message_id", envelope.message_id},
          {"sender_relay_id", envelope.sender_relay_id},
          {"body", std::move(body)},
          {"timestamp", envelope.timestamp},
          {"signature", envelope.signature}};
}

RelayEnvelope RelayEnvelopeFromJson(const nlohmann::json& json) {
  RelayEnvelope envelope;
  if (json.contains("thread_id") && json["thread_id"].is_string()) {
    envelope.thread_id = json["thread_id"].get<std::string>();
  }
  if (json.contains("message_id") && json["message_id"].is_string()) {
    envelope.message_id = json["message_id"].get<std::string>();
  }
  if (json.contains("sender_relay_id") && json["sender_relay_id"].is_string()) {
    envelope.sender_relay_id = json["sender_relay_id"].get<std::string>();
  }
  if (json.contains("timestamp") && json["timestamp"].is_number_integer()) {
    envelope.timestamp = json["timestamp"].get<int64_t>();
  }
  if (json.contains("signature") && json["signature"].is_string()) {
    envelope.signature = json["signature"].get<std::string>();
  }
  if (json.contains("body") && json["body"].is_object()) {
    const auto& body = json["body"];
    if (body.contains("text") && body["text"].is_string()) {
      envelope.body.text = body["text"].get<std::string>();
    }
    if (body.contains("content_rml") && body["content_rml"].is_string()) {
      envelope.body.content_rml = body["content_rml"].get<std::string>();
    }
  }
  return envelope;
}

std::string ContactIdKindToString(const ContactIdKind kind) {
  switch (kind) {
  case ContactIdKind::RelayUser:
    return "relay_user";
  case ContactIdKind::PeerId:
    return "peer_id";
  case ContactIdKind::Blockchain:
    return "blockchain";
  case ContactIdKind::Custom:
    return "custom";
  }
  return "relay_user";
}

ContactIdKind ContactIdKindFromString(const std::string& value) {
  if (value == "peer_id") {
    return ContactIdKind::PeerId;
  }
  if (value == "blockchain") {
    return ContactIdKind::Blockchain;
  }
  if (value == "custom") {
    return ContactIdKind::Custom;
  }
  return ContactIdKind::RelayUser;
}

std::string TrustLevelToString(const TrustLevel level) {
  switch (level) {
  case TrustLevel::Unknown:
    return "unknown";
  case TrustLevel::Friendly:
    return "friendly";
  case TrustLevel::Blocked:
    return "blocked";
  }
  return "unknown";
}

TrustLevel TrustLevelFromString(const std::string& value) {
  if (value == "friendly") {
    return TrustLevel::Friendly;
  }
  if (value == "blocked") {
    return TrustLevel::Blocked;
  }
  return TrustLevel::Unknown;
}

nlohmann::json ContactToJson(const Contact& contact) {
  nlohmann::json ids = nlohmann::json::array();
  for (const ContactId& id : contact.ids) {
    ids.push_back({{"kind", ContactIdKindToString(id.kind)}, {"value", id.value}, {"primary", id.primary}});
  }
  return {{"id", contact.id},
          {"display_name", contact.display_name},
          {"server_nickname", contact.server_nickname},
          {"ids", std::move(ids)},
          {"trust", TrustLevelToString(contact.trust)}};
}

Contact ContactFromJson(const nlohmann::json& json) {
  Contact contact;
  if (json.contains("id") && json["id"].is_string()) {
    contact.id = json["id"].get<std::string>();
  }
  if (json.contains("display_name") && json["display_name"].is_string()) {
    contact.display_name = json["display_name"].get<std::string>();
  }
  if (json.contains("server_nickname") && json["server_nickname"].is_string()) {
    contact.server_nickname = json["server_nickname"].get<std::string>();
  }
  if (json.contains("trust") && json["trust"].is_string()) {
    contact.trust = TrustLevelFromString(json["trust"].get<std::string>());
  }
  if (json.contains("ids") && json["ids"].is_array()) {
    for (const auto& item : json["ids"]) {
      ContactId id;
      if (item.contains("kind") && item["kind"].is_string()) {
        id.kind = ContactIdKindFromString(item["kind"].get<std::string>());
      }
      if (item.contains("value") && item["value"].is_string()) {
        id.value = item["value"].get<std::string>();
      }
      if (item.contains("primary") && item["primary"].is_boolean()) {
        id.primary = item["primary"].get<bool>();
      }
      contact.ids.push_back(std::move(id));
    }
  }
  return contact;
}

nlohmann::json DirectoryHitToJson(const DirectoryHit& hit) {
  nlohmann::json ids = nlohmann::json::array();
  for (const ContactId& id : hit.ids) {
    ids.push_back({{"kind", ContactIdKindToString(id.kind)}, {"value", id.value}, {"primary", id.primary}});
  }
  return {{"hit_id", hit.hit_id},
          {"display_name", hit.display_name},
          {"nickname", hit.nickname},
          {"ids", std::move(ids)}};
}

DirectoryHit DirectoryHitFromJson(const nlohmann::json& json) {
  DirectoryHit hit;
  if (json.contains("hit_id") && json["hit_id"].is_string()) {
    hit.hit_id = json["hit_id"].get<std::string>();
  }
  if (json.contains("display_name") && json["display_name"].is_string()) {
    hit.display_name = json["display_name"].get<std::string>();
  }
  if (json.contains("nickname") && json["nickname"].is_string()) {
    hit.nickname = json["nickname"].get<std::string>();
  }
  if (json.contains("ids") && json["ids"].is_array()) {
    for (const auto& item : json["ids"]) {
      ContactId id;
      if (item.contains("kind") && item["kind"].is_string()) {
        id.kind = ContactIdKindFromString(item["kind"].get<std::string>());
      }
      if (item.contains("value") && item["value"].is_string()) {
        id.value = item["value"].get<std::string>();
      }
      if (item.contains("primary") && item["primary"].is_boolean()) {
        id.primary = item["primary"].get<bool>();
      }
      hit.ids.push_back(std::move(id));
    }
  }
  return hit;
}

} // namespace pbr
