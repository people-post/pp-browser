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

std::string ThreadChannelToString(const ThreadChannel channel) {
  switch (channel) {
  case ThreadChannel::E2e:
    return "e2e";
  case ThreadChannel::E2ePublic:
    return "e2e_public";
  case ThreadChannel::None:
    return "";
  }
  return "";
}

ThreadChannel ThreadChannelFromString(const std::string& value) {
  if (value == "e2e_public") {
    return ThreadChannel::E2ePublic;
  }
  if (value == "e2e") {
    return ThreadChannel::E2e;
  }
  return ThreadChannel::None;
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

std::string MessageTransportToString(const MessageTransport transport) {
  switch (transport) {
  case MessageTransport::Relay:
    return "relay";
  case MessageTransport::Direct:
    return "direct";
  case MessageTransport::Local:
    return "local";
  }
  return "local";
}

MessageTransport MessageTransportFromString(const std::string& value) {
  if (value == "relay") {
    return MessageTransport::Relay;
  }
  if (value == "direct") {
    return MessageTransport::Direct;
  }
  return MessageTransport::Local;
}

nlohmann::json ThreadToJson(const Thread& thread) {
  nlohmann::json json = {{"id", thread.id},
                         {"kind", ThreadKindToString(thread.kind)},
                         {"title", thread.title},
                         {"participant_contact_ids", thread.participant_contact_ids},
                         {"updated_at", thread.updated_at},
                         {"unread_count", thread.unread_count},
                         {"preview", thread.preview},
                         {"encrypted", thread.encrypted}};
  if (thread.channel != ThreadChannel::None) {
    json["channel"] = ThreadChannelToString(thread.channel);
  }
  if (!thread.peer_identity_kind.empty()) {
    json["peer_identity_kind"] = thread.peer_identity_kind;
  }
  if (!thread.peer_identity_value.empty()) {
    json["peer_identity_value"] = thread.peer_identity_value;
  }
  return json;
}

Thread ThreadFromJson(const nlohmann::json& json) {
  Thread thread;
  if (json.contains("id") && json["id"].is_string()) {
    thread.id = json["id"].get<std::string>();
  }
  if (json.contains("kind") && json["kind"].is_string()) {
    thread.kind = ThreadKindFromString(json["kind"].get<std::string>());
  }
  if (json.contains("channel") && json["channel"].is_string()) {
    thread.channel = ThreadChannelFromString(json["channel"].get<std::string>());
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
  if (json.contains("peer_identity_kind") && json["peer_identity_kind"].is_string()) {
    thread.peer_identity_kind = json["peer_identity_kind"].get<std::string>();
  }
  if (json.contains("peer_identity_value") && json["peer_identity_value"].is_string()) {
    thread.peer_identity_value = json["peer_identity_value"].get<std::string>();
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
  if (json.contains("encrypted") && json["encrypted"].is_boolean()) {
    thread.encrypted = json["encrypted"].get<bool>();
  } else if (ThreadChannelIsE2e(thread.channel)) {
    thread.encrypted = true;
  }
  return thread;
}

nlohmann::json ThreadMessageToJson(const ThreadMessage& message) {
  nlohmann::json json = {{"id", message.id},
                         {"thread_id", message.thread_id},
                         {"sender_contact_id", message.sender_contact_id},
                         {"display_order", message.display_order},
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
  if (json.contains("display_order") && json["display_order"].is_number_integer()) {
    message.display_order = json["display_order"].get<int64_t>();
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
  nlohmann::json body = {{"e2e", {{"payload_b64", envelope.body.e2e.payload_b64}}}};
  if (envelope.body.e2e.key_init_b64) {
    body["e2e"]["key_init_b64"] = *envelope.body.e2e.key_init_b64;
  }
  nlohmann::json route = {{"kind", envelope.route.kind}, {"channel", ThreadChannelToString(envelope.route.channel)}};
  if (envelope.route.group_id) {
    route["group_id"] = *envelope.route.group_id;
  }
  return {{"envelope_version", envelope.envelope_version},
          {"message_id", envelope.message_id},
          {"sender_relay_id", envelope.sender_relay_id},
          {"sender_contact_id", envelope.sender_contact_id},
          {"route", std::move(route)},
          {"body", std::move(body)},
          {"sender_seq", envelope.sender_seq},
          {"session_epoch", envelope.session_epoch},
          {"timestamp", envelope.timestamp},
          {"signature", envelope.signature}};
}

Roe<RelayEnvelope> ParseRelayEnvelope(const nlohmann::json& json) {
  if (json.contains("thread_id")) {
    return Error("Legacy relay envelope thread_id is not supported");
  }
  if (!json.contains("envelope_version") || !json["envelope_version"].is_number_integer()) {
    return Error("Missing envelope_version");
  }
  const int version = json["envelope_version"].get<int>();
  if (version != kRelayEnvelopeVersion) {
    return Error("Unsupported envelope_version");
  }
  if (!json.contains("message_id") || !json["message_id"].is_string()) {
    return Error("Missing message_id");
  }
  if (!json.contains("sender_relay_id") || !json["sender_relay_id"].is_string()) {
    return Error("Missing sender_relay_id");
  }
  if (!json.contains("sender_contact_id") || !json["sender_contact_id"].is_string()) {
    return Error("Missing sender_contact_id");
  }
  if (!json.contains("route") || !json["route"].is_object()) {
    return Error("Missing route");
  }
  if (!json.contains("body") || !json["body"].is_object()) {
    return Error("Missing body");
  }
  const auto& body = json["body"];
  if (body.contains("text")) {
    return Error("Legacy relay body.text is not supported");
  }
  if (body.contains("content_b64")) {
    return Error("Legacy relay body.content_b64 is not supported");
  }
  if (body.contains("content_rml")) {
    return Error("Remote content_rml is not supported on wire");
  }
  if (body.contains("public_relay")) {
    return Error("public_relay body is not supported");
  }
  if (!body.contains("e2e") || !body["e2e"].is_object()) {
    return Error("Missing body.e2e");
  }
  const auto& e2e = body["e2e"];
  if (e2e.contains("content_rml")) {
    return Error("Remote content_rml is not supported on wire");
  }
  if (!e2e.contains("payload_b64") || !e2e["payload_b64"].is_string()) {
    return Error("Missing body.e2e.payload_b64");
  }

  RelayEnvelope envelope;
  envelope.envelope_version = version;
  envelope.message_id = json["message_id"].get<std::string>();
  envelope.sender_relay_id = json["sender_relay_id"].get<std::string>();
  envelope.sender_contact_id = json["sender_contact_id"].get<std::string>();
  envelope.body.e2e.payload_b64 = e2e["payload_b64"].get<std::string>();
  if (e2e.contains("key_init_b64") && e2e["key_init_b64"].is_string()) {
    envelope.body.e2e.key_init_b64 = e2e["key_init_b64"].get<std::string>();
  }

  const auto& route = json["route"];
  if (route.contains("kind") && route["kind"].is_string()) {
    envelope.route.kind = route["kind"].get<std::string>();
  }
  if (route.contains("channel") && route["channel"].is_string()) {
    const std::string channel_value = route["channel"].get<std::string>();
    if (channel_value == "public_relay") {
      return Error("public_relay channel is not supported");
    }
    envelope.route.channel = ThreadChannelFromString(channel_value);
    if (envelope.route.channel == ThreadChannel::None) {
      return Error("Invalid route.channel");
    }
  } else {
    return Error("Missing route.channel");
  }
  if (route.contains("group_id") && route["group_id"].is_string()) {
    envelope.route.group_id = route["group_id"].get<std::string>();
  }

  if (json.contains("sender_seq") && json["sender_seq"].is_number_unsigned()) {
    envelope.sender_seq = json["sender_seq"].get<uint64_t>();
  }
  if (json.contains("session_epoch") && json["session_epoch"].is_number_unsigned()) {
    envelope.session_epoch = json["session_epoch"].get<uint32_t>();
  }
  if (json.contains("timestamp") && json["timestamp"].is_number_integer()) {
    envelope.timestamp = json["timestamp"].get<int64_t>();
  }
  if (json.contains("signature") && json["signature"].is_string()) {
    envelope.signature = json["signature"].get<std::string>();
  }
  return envelope;
}

nlohmann::json ChatHistoryRequestToJson(const ChatHistoryRequest& request) {
  nlohmann::json json = {{"requester_identity_kind", request.requester_identity_kind},
                         {"requester_identity_value", request.requester_identity_value},
                         {"peer_identity_kind", request.peer_identity_kind},
                         {"peer_identity_value", request.peer_identity_value},
                         {"channel", ThreadChannelToString(request.channel)},
                         {"session_epoch", request.session_epoch},
                         {"limit", request.limit},
                         {"order", request.order}};
  if (request.min_sender_seq) {
    json["min_sender_seq"] = *request.min_sender_seq;
  }
  if (request.max_sender_seq) {
    json["max_sender_seq"] = *request.max_sender_seq;
  }
  return json;
}

Roe<ChatHistoryRequest> ChatHistoryRequestFromJson(const nlohmann::json& json) {
  ChatHistoryRequest request;
  if (!json.contains("requester_identity_kind") || !json.contains("requester_identity_value") ||
      !json.contains("peer_identity_kind") || !json.contains("peer_identity_value") ||
      !json.contains("channel") || !json.contains("session_epoch")) {
    return Error("Incomplete ChatHistoryRequest");
  }
  request.requester_identity_kind = json["requester_identity_kind"].get<std::string>();
  request.requester_identity_value = json["requester_identity_value"].get<std::string>();
  request.peer_identity_kind = json["peer_identity_kind"].get<std::string>();
  request.peer_identity_value = json["peer_identity_value"].get<std::string>();
  request.channel = ThreadChannelFromString(json["channel"].get<std::string>());
  request.session_epoch = json["session_epoch"].get<uint32_t>();
  if (json.contains("min_sender_seq") && json["min_sender_seq"].is_number_unsigned()) {
    request.min_sender_seq = json["min_sender_seq"].get<uint64_t>();
  }
  if (json.contains("max_sender_seq") && json["max_sender_seq"].is_number_unsigned()) {
    request.max_sender_seq = json["max_sender_seq"].get<uint64_t>();
  }
  if (json.contains("limit") && json["limit"].is_number_unsigned()) {
    request.limit = json["limit"].get<size_t>();
  }
  if (json.contains("order") && json["order"].is_string()) {
    request.order = json["order"].get<std::string>();
  }
  return request;
}

nlohmann::json ChatHistoryResponseToJson(const ChatHistoryResponse& response) {
  nlohmann::json messages = nlohmann::json::array();
  for (const RelayEnvelope& envelope : response.messages) {
    messages.push_back(RelayEnvelopeToJson(envelope));
  }
  nlohmann::json cursor = nlohmann::json::object();
  if (response.cursor.next_min_sender_seq) {
    cursor["next_min_sender_seq"] = *response.cursor.next_min_sender_seq;
  } else {
    cursor["next_min_sender_seq"] = nullptr;
  }
  if (response.cursor.next_max_sender_seq) {
    cursor["next_max_sender_seq"] = *response.cursor.next_max_sender_seq;
  } else {
    cursor["next_max_sender_seq"] = nullptr;
  }
  return {{"peer_identity_kind", response.peer_identity_kind},
          {"peer_identity_value", response.peer_identity_value},
          {"channel", ThreadChannelToString(response.channel)},
          {"session_epoch", response.session_epoch},
          {"messages", std::move(messages)},
          {"has_more", response.has_more},
          {"cursor", std::move(cursor)}};
}

Roe<ChatHistoryResponse> ChatHistoryResponseFromJson(const nlohmann::json& json) {
  ChatHistoryResponse response;
  if (!json.contains("peer_identity_kind") || !json.contains("peer_identity_value") ||
      !json.contains("channel") || !json.contains("session_epoch") || !json.contains("messages") ||
      !json.contains("has_more")) {
    return Error("Incomplete ChatHistoryResponse");
  }
  response.peer_identity_kind = json["peer_identity_kind"].get<std::string>();
  response.peer_identity_value = json["peer_identity_value"].get<std::string>();
  response.channel = ThreadChannelFromString(json["channel"].get<std::string>());
  response.session_epoch = json["session_epoch"].get<uint32_t>();
  response.has_more = json["has_more"].get<bool>();
  if (json["messages"].is_array()) {
    for (const auto& item : json["messages"]) {
      auto envelope = ParseRelayEnvelope(item);
      if (!envelope) {
        return envelope.error();
      }
      response.messages.push_back(*envelope);
    }
  }
  if (json.contains("cursor") && json["cursor"].is_object()) {
    const auto& cursor = json["cursor"];
    if (cursor.contains("next_min_sender_seq") && cursor["next_min_sender_seq"].is_number_unsigned()) {
      response.cursor.next_min_sender_seq = cursor["next_min_sender_seq"].get<uint64_t>();
    }
    if (cursor.contains("next_max_sender_seq") && cursor["next_max_sender_seq"].is_number_unsigned()) {
      response.cursor.next_max_sender_seq = cursor["next_max_sender_seq"].get<uint64_t>();
    }
  }
  return response;
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
