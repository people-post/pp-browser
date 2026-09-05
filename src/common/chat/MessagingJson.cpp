#include "common/chat/MessagingJson.h"

#include "common/thread/ThreadChannel.h"
#include "common/chat/RelayStreamKey.h"
#include "common/ValueJson.h"
#include "crypto/SodiumUtil.h"

#include <map>
#include <sstream>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

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

std::vector<TranscriptChatAction> ChatActionsFromJson(const Array& arr) {
  std::vector<TranscriptChatAction> out;
  for (const Value& item_value : arr.elements) {
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
    if (item->contains("payload")) {
      if (auto payload = item->getString("payload")) {
        action.payload = *payload;
      } else if (const Object* payload_obj = item->getObject("payload")) {
        // Older / in-memory shapes stored payload as a nested object.
        action.payload = DumpJson(*payload_obj);
      }
    }
    out.push_back(std::move(action));
  }
  return out;
}

Value StringArrayToJson(const std::vector<std::string>& values) {
  std::vector<Value> out;
  out.reserve(values.size());
  for (const std::string& value : values) {
    out.push_back(Value(value));
  }
  return ArrayValue(std::move(out));
}

Object BuildE2eBody(const RelayEnvelope& envelope) {
  Object e2e_body;
  if (envelope.body.e2e.member_payloads && !envelope.body.e2e.member_payloads->empty()) {
    Object payloads;
    for (const auto& [key, value] : *envelope.body.e2e.member_payloads) {
      payloads.set(key, value);
    }
    e2e_body.set("member_payloads", payloads);
  } else {
    e2e_body.set("payload_b64", envelope.body.e2e.payload_b64);
  }
  if (envelope.body.e2e.key_init_b64) {
    e2e_body.set("key_init_b64", *envelope.body.e2e.key_init_b64);
  }
  return e2e_body;
}

Object BuildRouteObject(const RelayEnvelope& envelope) {
  Object route;
  route.set("kind", envelope.route.kind);
  if (envelope.route.kind == "direct") {
    route.set("channel", ThreadChannelToString(envelope.route.channel));
  }
  if (envelope.route.group_id) {
    route.set("group_id", *envelope.route.group_id);
  }
  return route;
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

std::string MessageTransportBadgeLabel(const MessageTransport transport) {
  switch (transport) {
  case MessageTransport::Direct:
    return "Direct";
  case MessageTransport::Relay:
    return "Relay";
  case MessageTransport::Local:
    return "Local";
  }
  return "Local";
}

Object ThreadToJson(const Thread& thread) {
  Object json;
  json.set("id", thread.id);
  json.set("kind", ThreadKindToString(thread.kind));
  json.set("title", thread.title);
  json.set("participant_contact_ids", StringArrayToJson(thread.participant_contact_ids));
  json.set("updated_at", thread.updated_at);
  json.set("unread_count", static_cast<int64_t>(thread.unread_count));
  json.set("preview", thread.preview);
  json.set("encrypted", thread.encrypted);
  if (!thread.local_title.empty()) {
    json.set("local_title", thread.local_title);
  }
  if (thread.channel != ThreadChannel::None) {
    json.set("channel", ThreadChannelToString(thread.channel));
  }
  if (!thread.peer_identity_kind.empty()) {
    json.set("peer_identity_kind", thread.peer_identity_kind);
  }
  if (!thread.peer_identity_value.empty()) {
    json.set("peer_identity_value", thread.peer_identity_value);
  }
  return json;
}

Thread ThreadFromJson(const Object& json) {
  Thread thread;
  if (auto id = json.getString("id")) {
    thread.id = *id;
  }
  if (auto kind = json.getString("kind")) {
    thread.kind = ThreadKindFromString(*kind);
  }
  if (auto channel = json.getString("channel")) {
    thread.channel = ThreadChannelFromString(*channel);
  }
  if (auto title = json.getString("title")) {
    thread.title = *title;
  }
  if (auto local_title = json.getString("local_title")) {
    thread.local_title = *local_title;
  }
  if (const Array* ids = json.getArray("participant_contact_ids")) {
    for (const Value& id : ids->elements) {
      if (auto s = asString(id)) {
        thread.participant_contact_ids.push_back(*s);
      }
    }
  }
  if (auto kind = json.getString("peer_identity_kind")) {
    thread.peer_identity_kind = *kind;
  }
  if (auto value = json.getString("peer_identity_value")) {
    thread.peer_identity_value = *value;
  }
  if (auto updated_at = json.getIf<int64_t>("updated_at")) {
    thread.updated_at = *updated_at;
  }
  if (auto unread = json.getIf<int64_t>("unread_count")) {
    thread.unread_count = static_cast<int>(*unread);
  } else if (auto unread_u = json.getNonNegInt("unread_count")) {
    thread.unread_count = static_cast<int>(*unread_u);
  }
  if (auto preview = json.getString("preview")) {
    thread.preview = *preview;
  }
  if (auto encrypted = json.getIf<bool>("encrypted")) {
    thread.encrypted = *encrypted;
  } else if (ThreadChannelIsE2e(thread.channel)) {
    thread.encrypted = true;
  }
  return thread;
}

Object ThreadMessageToJson(const ThreadMessage& message) {
  Object json;
  json.set("id", message.id);
  json.set("thread_id", message.thread_id);
  json.set("sender_contact_id", message.sender_contact_id);
  json.set("display_order", message.display_order);
  json.set("text", message.text);
  json.set("timestamp", message.timestamp);
  json.set("delivery", MessageDeliveryToString(message.delivery));
  json.set("relay_visible", message.relay_visible);
  json.set("chat_actions", ChatActionsToJson(message.chat_actions));
  if (message.content_rml) {
    json.set("content_rml", *message.content_rml);
  }
  return json;
}

ThreadMessage ThreadMessageFromJson(const Object& json) {
  ThreadMessage message;
  if (auto id = json.getString("id")) {
    message.id = *id;
  }
  if (auto thread_id = json.getString("thread_id")) {
    message.thread_id = *thread_id;
  }
  if (auto sender = json.getString("sender_contact_id")) {
    message.sender_contact_id = *sender;
  }
  if (auto display_order = json.getIf<int64_t>("display_order")) {
    message.display_order = *display_order;
  }
  if (auto text = json.getString("text")) {
    message.text = *text;
  }
  if (auto content_rml = json.getString("content_rml")) {
    message.content_rml = *content_rml;
  }
  if (auto timestamp = json.getIf<int64_t>("timestamp")) {
    message.timestamp = *timestamp;
  }
  if (auto delivery = json.getString("delivery")) {
    message.delivery = MessageDeliveryFromString(*delivery);
  }
  if (auto relay_visible = json.getIf<bool>("relay_visible")) {
    message.relay_visible = *relay_visible;
  }
  if (const Array* actions = json.getArray("chat_actions")) {
    message.chat_actions = ChatActionsFromJson(*actions);
  }
  return message;
}

Object RelayEnvelopeToJson(const RelayEnvelope& envelope) {
  Object body;
  body.set("e2e", BuildE2eBody(envelope));

  Object json;
  json.set("op", "envelope");
  json.set("envelope_version", static_cast<int64_t>(envelope.envelope_version));
  json.set("message_id", envelope.message_id);
  json.set("sender_relay_id", envelope.sender_relay_id);
  json.set("sender_contact_id", envelope.sender_contact_id);
  json.set("route", BuildRouteObject(envelope));
  json.set("body", body);
  json.setJsonUInt("sender_seq", envelope.sender_seq);
  json.setJsonUInt("session_epoch", envelope.session_epoch);
  json.set("timestamp", envelope.timestamp);
  json.set("signature", envelope.signature);
  json.set("stream_key", envelope.stream_key);
  json.setJsonUInt("order_key", envelope.order_key);
  if (envelope.recipient_contact_id) {
    json.set("recipient_contact_id", *envelope.recipient_contact_id);
  }
  return json;
}

Object RelayEnvelopeToApplicationJson(const RelayEnvelope& envelope) {
  Object body;
  body.set("e2e", BuildE2eBody(envelope));

  Object json;
  json.set("envelope_version", static_cast<int64_t>(envelope.envelope_version));
  json.set("message_id", envelope.message_id);
  json.set("sender_relay_id", envelope.sender_relay_id);
  json.set("sender_contact_id", envelope.sender_contact_id);
  json.set("route", BuildRouteObject(envelope));
  json.set("body", body);
  json.setJsonUInt("sender_seq", envelope.sender_seq);
  json.setJsonUInt("session_epoch", envelope.session_epoch);
  json.set("timestamp", envelope.timestamp);
  json.set("signature", envelope.signature);
  return json;
}

Roe<RelayEnvelope> ParseRelayEnvelope(const Object& json) {
  if (json.contains("thread_id")) {
    return Error("Legacy relay envelope thread_id is not supported");
  }
  auto version_opt = json.getIf<int64_t>("envelope_version");
  if (!version_opt) {
    if (auto as_u = json.getNonNegInt("envelope_version")) {
      version_opt = static_cast<int64_t>(*as_u);
    }
  }
  if (!version_opt) {
    return Error("Missing envelope_version");
  }
  const int version = static_cast<int>(*version_opt);
  if (version != kRelayEnvelopeVersion) {
    return Error("Unsupported envelope_version");
  }
  auto message_id = json.getString("message_id");
  if (!message_id) {
    return Error("Missing message_id");
  }
  auto sender_relay_id = json.getString("sender_relay_id");
  if (!sender_relay_id) {
    return Error("Missing sender_relay_id");
  }
  auto sender_contact_id = json.getString("sender_contact_id");
  if (!sender_contact_id) {
    return Error("Missing sender_contact_id");
  }
  const Object* route = json.getObject("route");
  if (!route) {
    return Error("Missing route");
  }
  const Object* body = json.getObject("body");
  if (!body) {
    return Error("Missing body");
  }
  if (body->contains("text")) {
    return Error("Legacy relay body.text is not supported");
  }
  if (body->contains("content_b64")) {
    return Error("Legacy relay body.content_b64 is not supported");
  }
  if (body->contains("content_rml")) {
    return Error("Remote content_rml is not supported on wire");
  }
  if (body->contains("public_relay")) {
    return Error("public_relay body is not supported");
  }
  const Object* e2e = body->getObject("e2e");
  if (!e2e) {
    return Error("Missing body.e2e");
  }
  if (e2e->contains("content_rml")) {
    return Error("Remote content_rml is not supported on wire");
  }
  if (!e2e->contains("payload_b64") && !e2e->contains("member_payloads")) {
    return Error("Missing body.e2e.payload_b64 or member_payloads");
  }

  RelayEnvelope envelope;
  envelope.envelope_version = version;
  envelope.message_id = *message_id;
  envelope.sender_relay_id = *sender_relay_id;
  envelope.sender_contact_id = *sender_contact_id;
  if (auto payload_b64 = e2e->getString("payload_b64")) {
    envelope.body.e2e.payload_b64 = *payload_b64;
  }
  if (const Object* payloads = e2e->getObject("member_payloads")) {
    std::map<std::string, std::string> mapped;
    for (const auto& [key, value] : payloads->fields()) {
      if (auto s = asString(value)) {
        mapped[key] = *s;
      }
    }
    envelope.body.e2e.member_payloads = std::move(mapped);
  }
  if (auto key_init = e2e->getString("key_init_b64")) {
    envelope.body.e2e.key_init_b64 = *key_init;
  }

  if (auto kind = route->getString("kind")) {
    envelope.route.kind = *kind;
  }
  if (envelope.route.kind == "direct") {
    auto channel_value = route->getString("channel");
    if (!channel_value) {
      return Error("Missing route.channel");
    }
    if (*channel_value == "public_relay") {
      return Error("public_relay channel is not supported");
    }
    envelope.route.channel = ThreadChannelFromString(*channel_value);
    if (envelope.route.channel == ThreadChannel::None) {
      return Error("Invalid route.channel");
    }
  } else if (envelope.route.kind == "group") {
    envelope.route.channel = ThreadChannel::None;
  } else if (auto channel_value = route->getString("channel")) {
    envelope.route.channel = ThreadChannelFromString(*channel_value);
  }
  if (auto group_id = route->getString("group_id")) {
    envelope.route.group_id = *group_id;
  }
  if (envelope.route.kind == "group" && !envelope.route.group_id) {
    return Error("Missing route.group_id for group envelope");
  }

  if (auto sender_seq = json.getNonNegInt("sender_seq")) {
    envelope.sender_seq = *sender_seq;
  }
  if (auto session_epoch = json.getNonNegInt("session_epoch")) {
    envelope.session_epoch = static_cast<uint32_t>(*session_epoch);
  }
  if (auto timestamp = json.getIf<int64_t>("timestamp")) {
    envelope.timestamp = *timestamp;
  }
  if (auto signature = json.getString("signature")) {
    envelope.signature = *signature;
  }
  if (auto stream_key = json.getString("stream_key")) {
    envelope.stream_key = *stream_key;
  }
  if (auto order_key = json.getNonNegInt("order_key")) {
    envelope.order_key = *order_key;
  }
  if (auto recipient = json.getString("recipient_contact_id")) {
    envelope.recipient_contact_id = *recipient;
  }
  return envelope;
}

Object RelayWireSendRecordToJson(const RelayWireSendRecord& record) {
  Object json;
  json.set("sender_contact_id", record.sender_contact_id);
  json.set("recipient_contact_id", record.recipient_contact_id);
  json.set("stream_id", record.stream_id);
  json.setJsonUInt("index_key", record.index_key);
  json.set("blob_b64", record.blob_b64);
  json.set("timestamp", record.timestamp);
  json.set("signature", record.signature);
  return json;
}

Roe<RelayWireSendRecord> RelayWireSendRecordFromEnvelope(const RelayEnvelope& envelope) {
  if (envelope.sender_relay_id.empty()) {
    return Error("Missing sender_relay_id for relay send");
  }
  if (!envelope.recipient_contact_id || envelope.recipient_contact_id->empty()) {
    return Error("Missing recipient_contact_id for relay send");
  }
  if (envelope.stream_key.empty()) {
    return Error("Missing stream_key for relay send");
  }
  const uint64_t index_key = envelope.order_key != 0 ? envelope.order_key : envelope.sender_seq;
  const std::string serialized = DumpJson(RelayEnvelopeToApplicationJson(envelope));
  const ::pp::ByteVector bytes(serialized.begin(), serialized.end());
  RelayWireSendRecord record;
  // HTTP routing / auth id is always the relay registration id (WIRE_SCHEMAS RelayWireRecord).
  // Application blob keeps Account ID in envelope.sender_contact_id (D079).
  record.sender_contact_id = envelope.sender_relay_id;
  record.recipient_contact_id = *envelope.recipient_contact_id;
  record.stream_id = envelope.stream_key;
  record.index_key = index_key;
  record.blob_b64 = ::pp::Base64Encode(bytes);
  return record;
}

Roe<RelayInboundRecord> ParseRelayInboundRecord(const Object& json) {
  auto sender = json.getString("sender_contact_id");
  if (!sender) {
    return Error("Missing sender_contact_id");
  }
  auto stream_id = json.getString("stream_id");
  if (!stream_id) {
    return Error("Missing stream_id");
  }
  auto index_key = json.getNonNegInt("index_key");
  if (!index_key) {
    return Error("Missing index_key");
  }
  auto blob_b64 = json.getString("blob_b64");
  if (!blob_b64) {
    return Error("Missing blob_b64");
  }
  RelayInboundRecord record;
  record.sender_contact_id = *sender;
  record.stream_id = *stream_id;
  record.index_key = *index_key;
  record.blob_b64 = *blob_b64;
  if (auto created_at = json.getIf<int64_t>("created_at")) {
    record.created_at_ms = *created_at;
  }
  return record;
}

Roe<RelayEnvelope> RelayEnvelopeFromInboundRecord(const RelayInboundRecord& record) {
  auto decoded = ::pp::Base64Decode(record.blob_b64);
  if (!decoded) {
    return decoded.error();
  }
  const std::string serialized(decoded->begin(), decoded->end());
  auto app_json = TryParseObject(serialized);
  if (!app_json) {
    return Error("Invalid relay blob JSON");
  }
  auto envelope = ParseRelayEnvelope(*app_json);
  if (!envelope) {
    return envelope.error();
  }
  envelope->stream_key = record.stream_id;
  envelope->order_key = record.index_key;
  envelope->relay_created_at_ms = record.created_at_ms;
  return *envelope;
}

Object ChatHistoryRequestToJson(const ChatHistoryRequest& request) {
  Object json;
  json.set("op", "history");
  json.set("requester_identity_kind", request.requester_identity_kind);
  json.set("requester_identity_value", request.requester_identity_value);
  json.set("peer_identity_kind", request.peer_identity_kind);
  json.set("peer_identity_value", request.peer_identity_value);
  json.set("channel", ThreadChannelToString(request.channel));
  json.setJsonUInt("session_epoch", request.session_epoch);
  json.setJsonUInt("limit", static_cast<uint64_t>(request.limit));
  json.set("order", request.order);
  if (request.min_sender_seq) {
    json.setJsonUInt("min_sender_seq", *request.min_sender_seq);
  }
  if (request.max_sender_seq) {
    json.setJsonUInt("max_sender_seq", *request.max_sender_seq);
  }
  return json;
}

Roe<ChatHistoryRequest> ChatHistoryRequestFromJson(const Object& json) {
  ChatHistoryRequest request;
  auto requester_kind = json.getString("requester_identity_kind");
  auto requester_value = json.getString("requester_identity_value");
  auto peer_kind = json.getString("peer_identity_kind");
  auto peer_value = json.getString("peer_identity_value");
  auto channel = json.getString("channel");
  auto session_epoch = json.getNonNegInt("session_epoch");
  if (!requester_kind || !requester_value || !peer_kind || !peer_value || !channel || !session_epoch) {
    return Error("Incomplete ChatHistoryRequest");
  }
  request.requester_identity_kind = *requester_kind;
  request.requester_identity_value = *requester_value;
  request.peer_identity_kind = *peer_kind;
  request.peer_identity_value = *peer_value;
  request.channel = ThreadChannelFromString(*channel);
  request.session_epoch = static_cast<uint32_t>(*session_epoch);
  if (auto min_seq = json.getNonNegInt("min_sender_seq")) {
    request.min_sender_seq = *min_seq;
  }
  if (auto max_seq = json.getNonNegInt("max_sender_seq")) {
    request.max_sender_seq = *max_seq;
  }
  if (auto limit = json.getNonNegInt("limit")) {
    request.limit = static_cast<size_t>(*limit);
  }
  if (auto order = json.getString("order")) {
    request.order = *order;
  }
  return request;
}

std::string StreamHistoryRequestToQueryString(const ChatHistoryRequest& request) {
  const std::string stream_key =
      BuildCanonicalRelayStreamKey(request.requester_identity_value, request.peer_identity_value, request.channel,
                                 request.session_epoch);
  std::ostringstream out;
  bool first = true;
  auto append = [&](const char* key, const std::string& value) {
    out << (first ? '?' : '&') << key << '=' << value;
    first = false;
  };
  append("requester_contact_id", request.requester_identity_value);
  append("sender_contact_id", request.peer_identity_value);
  append("stream_id", stream_key);
  append("limit", std::to_string(request.limit));
  append("order", request.order);
  if (request.min_sender_seq) {
    append("min_index_key", std::to_string(*request.min_sender_seq));
  }
  if (request.max_sender_seq) {
    append("max_index_key", std::to_string(*request.max_sender_seq));
  }
  return out.str();
}

std::string ChatHistoryRequestToQueryString(const ChatHistoryRequest& request) {
  return StreamHistoryRequestToQueryString(request);
}

Object ChatHistoryRequestToStreamHistoryJson(const ChatHistoryRequest& request) {
  Object json;
  json.set("requester_contact_id", request.requester_identity_value);
  json.set("sender_contact_id", request.peer_identity_value);
  json.set("stream_id", BuildCanonicalRelayStreamKey(request.requester_identity_value, request.peer_identity_value,
                                                     request.channel, request.session_epoch));
  json.setJsonUInt("limit", static_cast<uint64_t>(request.limit));
  json.set("order", request.order);
  if (request.min_sender_seq) {
    json.setJsonUInt("min_index_key", *request.min_sender_seq);
  }
  if (request.max_sender_seq) {
    json.setJsonUInt("max_index_key", *request.max_sender_seq);
  }
  return json;
}

Object ChatHistoryResponseToJson(const ChatHistoryResponse& response) {
  std::vector<Value> messages;
  messages.reserve(response.messages.size());
  for (const RelayEnvelope& envelope : response.messages) {
    messages.push_back(ObjectValue(RelayEnvelopeToJson(envelope)));
  }
  Object cursor;
  if (response.cursor.next_min_sender_seq) {
    cursor.setJsonUInt("next_min_sender_seq", *response.cursor.next_min_sender_seq);
  } else {
    cursor.set("next_min_sender_seq", Null{});
  }
  if (response.cursor.next_max_sender_seq) {
    cursor.setJsonUInt("next_max_sender_seq", *response.cursor.next_max_sender_seq);
  } else {
    cursor.set("next_max_sender_seq", Null{});
  }
  Object json;
  json.set("peer_identity_kind", response.peer_identity_kind);
  json.set("peer_identity_value", response.peer_identity_value);
  json.set("channel", ThreadChannelToString(response.channel));
  json.setJsonUInt("session_epoch", response.session_epoch);
  json.set("messages", ArrayValue(std::move(messages)));
  json.set("has_more", response.has_more);
  json.set("cursor", cursor);
  return json;
}

Roe<ChatHistoryResponse> ChatHistoryResponseFromJson(const Object& json) {
  ChatHistoryResponse response;
  const bool stream_shape =
      (json.contains("stream_id") || json.contains("stream_key")) && json.contains("sender_contact_id");
  const bool chat_shape = json.contains("peer_identity_kind") && json.contains("peer_identity_value");
  if (!json.contains("messages") || !json.contains("has_more") || (!stream_shape && !chat_shape)) {
    return Error("Incomplete ChatHistoryResponse");
  }
  if (chat_shape) {
    if (auto kind = json.getString("peer_identity_kind")) {
      response.peer_identity_kind = *kind;
    }
    if (auto value = json.getString("peer_identity_value")) {
      response.peer_identity_value = *value;
    }
    if (auto channel = json.getString("channel")) {
      response.channel = ThreadChannelFromString(*channel);
    }
    if (auto session_epoch = json.getNonNegInt("session_epoch")) {
      response.session_epoch = static_cast<uint32_t>(*session_epoch);
    }
  } else {
    if (auto sender = json.getString("sender_contact_id")) {
      response.peer_identity_value = *sender;
    }
  }
  if (auto has_more = json.getIf<bool>("has_more")) {
    response.has_more = *has_more;
  } else {
    return Error("Incomplete ChatHistoryResponse");
  }
  if (const Array* messages = json.getArray("messages")) {
    for (const Value& item_value : messages->elements) {
      const Object* item = asObject(item_value);
      if (!item) {
        continue;
      }
      if (item->contains("blob_b64")) {
        auto inbound = ParseRelayInboundRecord(*item);
        if (!inbound) {
          return inbound.error();
        }
        auto envelope = RelayEnvelopeFromInboundRecord(*inbound);
        if (!envelope) {
          return envelope.error();
        }
        response.messages.push_back(*envelope);
      } else {
        auto envelope = ParseRelayEnvelope(*item);
        if (!envelope) {
          return envelope.error();
        }
        response.messages.push_back(*envelope);
      }
    }
  }
  if (const Object* cursor = json.getObject("cursor")) {
    // Distinct names per branch: MSVC C4456 (/WX) treats if-init reuse in
    // else-if as hiding the previous declaration.
    if (auto next_min_seq = cursor->getNonNegInt("next_min_sender_seq")) {
      response.cursor.next_min_sender_seq = *next_min_seq;
    } else if (auto next_min_index = cursor->getNonNegInt("next_min_index_key")) {
      response.cursor.next_min_sender_seq = *next_min_index;
    } else if (auto next_min_order = cursor->getNonNegInt("next_min_order_key")) {
      response.cursor.next_min_sender_seq = *next_min_order;
    }
    if (auto next_max_seq = cursor->getNonNegInt("next_max_sender_seq")) {
      response.cursor.next_max_sender_seq = *next_max_seq;
    } else if (auto next_max_index = cursor->getNonNegInt("next_max_index_key")) {
      response.cursor.next_max_sender_seq = *next_max_index;
    } else if (auto next_max_order = cursor->getNonNegInt("next_max_order_key")) {
      response.cursor.next_max_sender_seq = *next_max_order;
    }
  }
  return response;
}

std::string ChatBlobOpToString(const ChatBlobOp op) {
  switch (op) {
  case ChatBlobOp::Fetch:
    return "fetch";
  case ChatBlobOp::Push:
    return "push";
  }
  return "fetch";
}

ChatBlobOp ChatBlobOpFromString(const std::string& value) {
  if (value == "push") {
    return ChatBlobOp::Push;
  }
  return ChatBlobOp::Fetch;
}

Object ChatBlobRequestToJson(const ChatBlobRequest& request) {
  Object json;
  json.set("op", ChatBlobOpToString(request.op));
  json.set("requester_identity_kind", request.requester_identity_kind);
  json.set("requester_identity_value", request.requester_identity_value);
  json.set("peer_identity_kind", request.peer_identity_kind);
  json.set("peer_identity_value", request.peer_identity_value);
  json.set("thread_id", request.thread_id);
  json.set("content_hash_hex", request.content_hash_hex);
  json.set("channel", ThreadChannelToString(request.channel));
  return json;
}

Roe<ChatBlobRequest> ChatBlobRequestFromJson(const Object& json) {
  ChatBlobRequest request;
  auto requester_kind = json.getString("requester_identity_kind");
  auto requester_value = json.getString("requester_identity_value");
  auto peer_kind = json.getString("peer_identity_kind");
  auto peer_value = json.getString("peer_identity_value");
  auto thread_id = json.getString("thread_id");
  auto content_hash = json.getString("content_hash_hex");
  auto channel = json.getString("channel");
  if (!requester_kind || !requester_value || !peer_kind || !peer_value || !thread_id || !content_hash || !channel) {
    return Error("Incomplete ChatBlobRequest");
  }
  if (auto op = json.getString("op")) {
    request.op = ChatBlobOpFromString(*op);
  }
  request.requester_identity_kind = *requester_kind;
  request.requester_identity_value = *requester_value;
  request.peer_identity_kind = *peer_kind;
  request.peer_identity_value = *peer_value;
  request.thread_id = *thread_id;
  request.content_hash_hex = *content_hash;
  request.channel = ThreadChannelFromString(*channel);
  return request;
}

Object ChatBlobAckToJson(const bool ok, const std::string& error) {
  Object json;
  json.set("ok", ok);
  if (!error.empty()) {
    json.set("error", error);
  }
  return json;
}

} // namespace pbr
