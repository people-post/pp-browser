#include "base/messaging/CallControlCodec.h"

#include "common/Utilities.h"

#include <nlohmann/json.hpp>

namespace pbr {
namespace {

nlohmann::json ParseObject(const std::string& detail_json) {
  return nlohmann::json::parse(detail_json, nullptr, false);
}

std::optional<std::string> OptString(const nlohmann::json& json, const char* key) {
  if (!json.contains(key) || !json[key].is_string()) {
    return std::nullopt;
  }
  return json[key].get<std::string>();
}

std::optional<int64_t> OptInt64(const nlohmann::json& json, const char* key) {
  if (!json.contains(key) || !json[key].is_number_integer()) {
    return std::nullopt;
  }
  return json[key].get<int64_t>();
}

} // namespace

Roe<std::string> CallControlCodec::EncodeInvite(const CallInviteDetail& detail) {
  nlohmann::json json{{"call_id", detail.call_id},
                      {"inviter_identity", detail.inviter_identity},
                      {"invitee_identity", detail.invitee_identity},
                      {"media_mode", CallMediaModeToString(detail.media_mode)}};
  if (detail.origin_thread_id) {
    json["origin_thread_id"] = *detail.origin_thread_id;
  }
  if (detail.origin_group_id) {
    json["origin_group_id"] = *detail.origin_group_id;
  }
  if (detail.sfu_hint) {
    json["sfu_hint"] = *detail.sfu_hint;
  }
  if (detail.expires_at) {
    json["expires_at"] = *detail.expires_at;
  }
  return json.dump();
}

Roe<CallInviteDetail> CallControlCodec::DecodeInvite(const std::string& detail_json) {
  const nlohmann::json json = ParseObject(detail_json);
  if (!json.is_object() || !json.contains("call_id") || !json["call_id"].is_string()) {
    return Error("Invalid call_invite detail");
  }
  CallInviteDetail detail;
  detail.call_id = json["call_id"].get<std::string>();
  detail.inviter_identity = OptString(json, "inviter_identity").value_or("");
  detail.invitee_identity = OptString(json, "invitee_identity").value_or("");
  detail.media_mode = CallMediaModeFromString(OptString(json, "media_mode").value_or("voice"));
  detail.origin_thread_id = OptString(json, "origin_thread_id");
  detail.origin_group_id = OptString(json, "origin_group_id");
  detail.sfu_hint = OptString(json, "sfu_hint");
  detail.expires_at = OptInt64(json, "expires_at");
  return detail;
}

Roe<std::string> CallControlCodec::EncodeAccept(const CallAcceptDetail& detail) {
  return nlohmann::json({{"call_id", detail.call_id},
                         {"identity", detail.identity},
                         {"audio_muted", detail.audio_muted},
                         {"video_enabled", detail.video_enabled}})
      .dump();
}

Roe<CallAcceptDetail> CallControlCodec::DecodeAccept(const std::string& detail_json) {
  const nlohmann::json json = ParseObject(detail_json);
  if (!json.is_object() || !json.contains("call_id") || !json["call_id"].is_string()) {
    return Error("Invalid call_accept detail");
  }
  CallAcceptDetail detail;
  detail.call_id = json["call_id"].get<std::string>();
  detail.identity = OptString(json, "identity").value_or("");
  detail.audio_muted = json.value("audio_muted", false);
  detail.video_enabled = json.value("video_enabled", false);
  return detail;
}

Roe<std::string> CallControlCodec::EncodeDecline(const CallDeclineDetail& detail) {
  return nlohmann::json({{"call_id", detail.call_id}, {"identity", detail.identity}}).dump();
}

Roe<CallDeclineDetail> CallControlCodec::DecodeDecline(const std::string& detail_json) {
  const nlohmann::json json = ParseObject(detail_json);
  if (!json.is_object() || !json.contains("call_id") || !json["call_id"].is_string()) {
    return Error("Invalid call_decline detail");
  }
  CallDeclineDetail detail;
  detail.call_id = json["call_id"].get<std::string>();
  detail.identity = OptString(json, "identity").value_or("");
  return detail;
}

Roe<std::string> CallControlCodec::EncodeLeave(const CallLeaveDetail& detail) {
  return nlohmann::json({{"call_id", detail.call_id}, {"identity", detail.identity}}).dump();
}

Roe<CallLeaveDetail> CallControlCodec::DecodeLeave(const std::string& detail_json) {
  const nlohmann::json json = ParseObject(detail_json);
  if (!json.is_object() || !json.contains("call_id") || !json["call_id"].is_string()) {
    return Error("Invalid call_leave detail");
  }
  CallLeaveDetail detail;
  detail.call_id = json["call_id"].get<std::string>();
  detail.identity = OptString(json, "identity").value_or("");
  return detail;
}

Roe<std::string> CallControlCodec::EncodeRoster(const CallRosterDetail& detail) {
  nlohmann::json participants = nlohmann::json::array();
  for (const CallRosterEntry& entry : detail.participants) {
    participants.push_back(nlohmann::json{{"identity", entry.identity},
                                          {"state", CallParticipantStateToString(entry.state)},
                                          {"audio_muted", entry.audio_muted},
                                          {"video_enabled", entry.video_enabled}});
  }
  return nlohmann::json({{"call_id", detail.call_id},
                         {"media_epoch", detail.media_epoch},
                         {"participants", std::move(participants)}})
      .dump();
}

Roe<CallRosterDetail> CallControlCodec::DecodeRoster(const std::string& detail_json) {
  const nlohmann::json json = ParseObject(detail_json);
  if (!json.is_object() || !json.contains("call_id") || !json["call_id"].is_string()) {
    return Error("Invalid call_roster detail");
  }
  CallRosterDetail detail;
  detail.call_id = json["call_id"].get<std::string>();
  detail.media_epoch = static_cast<uint32_t>(json.value("media_epoch", 1));
  if (json.contains("participants") && json["participants"].is_array()) {
    for (const nlohmann::json& row : json["participants"]) {
      if (!row.is_object() || !row.contains("identity") || !row["identity"].is_string()) {
        continue;
      }
      CallRosterEntry entry;
      entry.identity = row["identity"].get<std::string>();
      entry.state = CallParticipantStateFromString(row.value("state", "joined"));
      entry.audio_muted = row.value("audio_muted", false);
      entry.video_enabled = row.value("video_enabled", false);
      detail.participants.push_back(std::move(entry));
    }
  }
  return detail;
}

Roe<std::string> CallControlCodec::EncodeMediaKey(const CallMediaKeyDetail& detail) {
  return nlohmann::json({{"call_id", detail.call_id},
                         {"media_epoch", detail.media_epoch},
                         {"media_key_id", detail.media_key_id},
                         {"wrapped_key_b64", detail.wrapped_key_b64}})
      .dump();
}

Roe<CallMediaKeyDetail> CallControlCodec::DecodeMediaKey(const std::string& detail_json) {
  const nlohmann::json json = ParseObject(detail_json);
  if (!json.is_object() || !json.contains("call_id") || !json["call_id"].is_string()) {
    return Error("Invalid call_media_key detail");
  }
  CallMediaKeyDetail detail;
  detail.call_id = json["call_id"].get<std::string>();
  detail.media_epoch = static_cast<uint32_t>(json.value("media_epoch", 1));
  detail.media_key_id = OptString(json, "media_key_id").value_or("");
  detail.wrapped_key_b64 = OptString(json, "wrapped_key_b64").value_or("");
  return detail;
}

Roe<std::string> CallControlCodec::EncodeEnded(const CallEndedDetail& detail) {
  nlohmann::json json{{"call_id", detail.call_id}};
  if (detail.duration_ms) {
    json["duration_ms"] = *detail.duration_ms;
  }
  return json.dump();
}

Roe<CallEndedDetail> CallControlCodec::DecodeEnded(const std::string& detail_json) {
  const nlohmann::json json = ParseObject(detail_json);
  if (!json.is_object() || !json.contains("call_id") || !json["call_id"].is_string()) {
    return Error("Invalid call_ended detail");
  }
  CallEndedDetail detail;
  detail.call_id = json["call_id"].get<std::string>();
  detail.duration_ms = OptInt64(json, "duration_ms");
  return detail;
}

Roe<std::string> CallControlCodec::EncodeStarted(const CallStartedDetail& detail) {
  return nlohmann::json({{"call_id", detail.call_id}, {"media_mode", CallMediaModeToString(detail.media_mode)}})
      .dump();
}

Roe<CallStartedDetail> CallControlCodec::DecodeStarted(const std::string& detail_json) {
  const nlohmann::json json = ParseObject(detail_json);
  if (!json.is_object() || !json.contains("call_id") || !json["call_id"].is_string()) {
    return Error("Invalid call_started detail");
  }
  CallStartedDetail detail;
  detail.call_id = json["call_id"].get<std::string>();
  detail.media_mode = CallMediaModeFromString(OptString(json, "media_mode").value_or("voice"));
  return detail;
}

Roe<ThreadMessage> CallControlCodec::BuildSystemMessage(const std::string& thread_id, const CallControlType type,
                                                        const std::string& display_text,
                                                        const std::string& detail_json,
                                                        const std::string& sender_contact_id) {
  ThreadMessage message;
  message.id = util::GenerateUuid();
  message.thread_id = thread_id;
  message.sender_contact_id = sender_contact_id;
  message.content_type = ChatContentType::System;
  message.text = display_text;
  message.payload_json =
      nlohmann::json({{"control_type", CallControlTypeToWire(type)}, {"detail", detail_json}}).dump();
  message.timestamp = util::NowUnixMs();
  message.delivery = MessageDelivery::Local;
  message.relay_visible = false;
  return message;
}

std::optional<CallControlType> CallControlCodec::ControlTypeFromMessage(const ThreadMessage& message) {
  if (message.content_type != ChatContentType::System || message.payload_json.empty()) {
    return std::nullopt;
  }
  const nlohmann::json json = nlohmann::json::parse(message.payload_json, nullptr, false);
  if (!json.is_object() || !json.contains("control_type") || !json["control_type"].is_string()) {
    return std::nullopt;
  }
  return CallControlTypeFromWire(json["control_type"].get<std::string>());
}

bool CallControlCodec::IsCallControlMessage(const ThreadMessage& message) {
  return ControlTypeFromMessage(message).has_value();
}

} // namespace pbr
