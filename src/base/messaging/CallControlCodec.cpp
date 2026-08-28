#include "base/messaging/CallControlCodec.h"

#include "common/Utilities.h"
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {
namespace {

std::vector<std::string> ReadStringArray(const Object& json, const char* key) {
  std::vector<std::string> out;
  const Array* arr = json.getArray(key);
  if (!arr) {
    return out;
  }
  for (const Value& row : arr->elements) {
    if (auto value = asString(row)) {
      if (!value->empty()) {
        out.push_back(*value);
      }
    }
  }
  return out;
}

void WriteStringArray(Object& json, const char* key, const std::vector<std::string>& values) {
  if (values.empty()) {
    return;
  }
  std::vector<Value> arr;
  for (const std::string& value : values) {
    if (!value.empty()) {
      arr.push_back(Value(value));
    }
  }
  if (!arr.empty()) {
    json.set(key, ArrayValue(std::move(arr)));
  }
}

void WritePeerCaps(Object& json, const CallPeerCaps& caps) {
  // Always emit when encoding from a build that knows caps (caller sets present or media_relay).
  if (!caps.present && !caps.media_relay) {
    return;
  }
  Object caps_obj;
  caps_obj.set("v", static_cast<int64_t>(caps.v > 0 ? caps.v : kCallPeerCapsVersion));
  caps_obj.set("media_relay", caps.media_relay);
  json.set("caps", ObjectValue(std::move(caps_obj)));
}

CallPeerCaps ReadPeerCaps(const Object& json) {
  CallPeerCaps caps;
  const Object* obj = json.getObject("caps");
  if (!obj) {
    return caps;
  }
  caps.present = true;
  caps.v = static_cast<int>(obj->getIf<int64_t>("v").value_or(kCallPeerCapsVersion));
  if (caps.v > kCallPeerCapsVersion) {
    // Newer schema we cannot interpret — unusable for hop pick.
    caps.media_relay = false;
    return caps;
  }
  caps.media_relay = obj->getIf<bool>("media_relay").value_or(false);
  return caps;
}

Object EncodeRosterEntry(const CallRosterEntry& entry) {
  Object row;
  row.set("identity", entry.identity);
  row.set("state", CallParticipantStateToString(entry.state));
  row.set("audio_muted", entry.audio_muted);
  row.set("video_enabled", entry.video_enabled);
  if (entry.joined_at) {
    row.set("joined_at", *entry.joined_at);
  }
  return row;
}

void ReadParticipants(const Object& json, std::vector<CallRosterEntry>& participants) {
  const Array* arr = json.getArray("participants");
  if (!arr) {
    return;
  }
  for (const Value& row_value : arr->elements) {
    const Object* row = asObject(row_value);
    if (!row) {
      continue;
    }
    auto identity = row->getString("identity");
    if (!identity) {
      continue;
    }
    CallRosterEntry entry;
    entry.identity = *identity;
    entry.state = CallParticipantStateFromString(row->getString("state").value_or("joined"));
    entry.audio_muted = row->getIf<bool>("audio_muted").value_or(false);
    entry.video_enabled = row->getIf<bool>("video_enabled").value_or(false);
    entry.joined_at = row->getIf<int64_t>("joined_at");
    participants.push_back(std::move(entry));
  }
}

} // namespace

Roe<std::string> CallControlCodec::EncodeInvite(const CallInviteDetail& detail) {
  Object json;
  json.set("call_id", detail.call_id);
  json.set("inviter_identity", detail.inviter_identity);
  json.set("invitee_identity", detail.invitee_identity);
  json.set("media_mode", CallMediaModeToString(detail.media_mode));
  if (detail.origin_thread_id) {
    json.set("origin_thread_id", *detail.origin_thread_id);
  }
  if (detail.origin_group_id) {
    json.set("origin_group_id", *detail.origin_group_id);
  }
  if (detail.sfu_hint) {
    json.set("sfu_hint", *detail.sfu_hint);
  }
  if (detail.expires_at) {
    json.set("expires_at", *detail.expires_at);
  }
  if (!detail.participants.empty()) {
    std::vector<Value> participants;
    participants.reserve(detail.participants.size());
    for (const CallRosterEntry& entry : detail.participants) {
      participants.push_back(ObjectValue(EncodeRosterEntry(entry)));
    }
    json.set("participants", ArrayValue(std::move(participants)));
  }
  if (!detail.wrapped_key_b64.empty()) {
    json.setJsonUInt("media_epoch", detail.media_epoch);
    json.set("media_key_id", detail.media_key_id);
    json.set("wrapped_key_b64", detail.wrapped_key_b64);
  }
  WriteStringArray(json, "listen_multiaddrs", detail.listen_multiaddrs);
  if (!detail.libp2p_peer_id.empty()) {
    json.set("libp2p_peer_id", detail.libp2p_peer_id);
  }
  WritePeerCaps(json, detail.caps);
  if (detail.offer_amount_minor > 0 || detail.floor_minor > 0) {
    json.set("offer_amount_minor", detail.offer_amount_minor);
    json.set("floor_minor", detail.floor_minor);
    json.set("currency", detail.currency.empty() ? "pp_credit" : detail.currency);
  }
  json.set("video_allowed", detail.video_allowed);
  return DumpJson(json);
}

Roe<CallInviteDetail> CallControlCodec::DecodeInvite(const std::string& detail_json) {
  auto json = TryParseObject(detail_json);
  auto call_id = json ? json->getString("call_id") : std::nullopt;
  if (!json || !call_id) {
    return Error("Invalid call_invite detail");
  }
  CallInviteDetail detail;
  detail.call_id = *call_id;
  detail.inviter_identity = json->getString("inviter_identity").value_or("");
  detail.invitee_identity = json->getString("invitee_identity").value_or("");
  detail.media_mode = CallMediaModeFromString(json->getString("media_mode").value_or("voice"));
  detail.origin_thread_id = json->getString("origin_thread_id");
  detail.origin_group_id = json->getString("origin_group_id");
  detail.sfu_hint = json->getString("sfu_hint");
  detail.expires_at = json->getIf<int64_t>("expires_at");
  ReadParticipants(*json, detail.participants);
  detail.media_epoch = static_cast<uint32_t>(json->getNonNegInt("media_epoch").value_or(1));
  detail.media_key_id = json->getString("media_key_id").value_or("");
  detail.wrapped_key_b64 = json->getString("wrapped_key_b64").value_or("");
  detail.listen_multiaddrs = ReadStringArray(*json, "listen_multiaddrs");
  detail.libp2p_peer_id = json->getString("libp2p_peer_id").value_or("");
  detail.caps = ReadPeerCaps(*json);
  detail.offer_amount_minor = json->getIf<int64_t>("offer_amount_minor").value_or(0);
  detail.floor_minor = json->getIf<int64_t>("floor_minor").value_or(0);
  detail.currency = json->getString("currency").value_or("pp_credit");
  if (json->contains("video_allowed")) {
    detail.video_allowed = json->getIf<bool>("video_allowed").value_or(false);
  } else {
    detail.video_allowed = detail.media_mode == CallMediaMode::Video;
  }
  return detail;
}

Roe<std::string> CallControlCodec::EncodeAccept(const CallAcceptDetail& detail) {
  Object json;
  json.set("call_id", detail.call_id);
  json.set("identity", detail.identity);
  json.set("audio_muted", detail.audio_muted);
  json.set("video_enabled", detail.video_enabled);
  WriteStringArray(json, "listen_multiaddrs", detail.listen_multiaddrs);
  if (!detail.libp2p_peer_id.empty()) {
    json.set("libp2p_peer_id", detail.libp2p_peer_id);
  }
  WritePeerCaps(json, detail.caps);
  if (detail.offer_amount_minor > 0 || detail.charge_decision == "take_all") {
    json.set("charge_decision", detail.charge_decision.empty() ? "waive" : detail.charge_decision);
    json.set("offer_amount_minor", detail.offer_amount_minor);
  }
  return DumpJson(json);
}

Roe<CallAcceptDetail> CallControlCodec::DecodeAccept(const std::string& detail_json) {
  auto json = TryParseObject(detail_json);
  auto call_id = json ? json->getString("call_id") : std::nullopt;
  if (!json || !call_id) {
    return Error("Invalid call_accept detail");
  }
  CallAcceptDetail detail;
  detail.call_id = *call_id;
  detail.identity = json->getString("identity").value_or("");
  detail.audio_muted = json->getIf<bool>("audio_muted").value_or(false);
  detail.video_enabled = json->getIf<bool>("video_enabled").value_or(false);
  detail.listen_multiaddrs = ReadStringArray(*json, "listen_multiaddrs");
  detail.libp2p_peer_id = json->getString("libp2p_peer_id").value_or("");
  detail.caps = ReadPeerCaps(*json);
  detail.charge_decision = json->getString("charge_decision").value_or("waive");
  detail.offer_amount_minor = json->getIf<int64_t>("offer_amount_minor").value_or(0);
  return detail;
}

Roe<std::string> CallControlCodec::EncodeDecline(const CallDeclineDetail& detail) {
  Object json;
  json.set("call_id", detail.call_id);
  json.set("identity", detail.identity);
  return DumpJson(json);
}

Roe<CallDeclineDetail> CallControlCodec::DecodeDecline(const std::string& detail_json) {
  auto json = TryParseObject(detail_json);
  auto call_id = json ? json->getString("call_id") : std::nullopt;
  if (!json || !call_id) {
    return Error("Invalid call_decline detail");
  }
  CallDeclineDetail detail;
  detail.call_id = *call_id;
  detail.identity = json->getString("identity").value_or("");
  return detail;
}

Roe<std::string> CallControlCodec::EncodeLeave(const CallLeaveDetail& detail) {
  Object json;
  json.set("call_id", detail.call_id);
  json.set("identity", detail.identity);
  return DumpJson(json);
}

Roe<CallLeaveDetail> CallControlCodec::DecodeLeave(const std::string& detail_json) {
  auto json = TryParseObject(detail_json);
  auto call_id = json ? json->getString("call_id") : std::nullopt;
  if (!json || !call_id) {
    return Error("Invalid call_leave detail");
  }
  CallLeaveDetail detail;
  detail.call_id = *call_id;
  detail.identity = json->getString("identity").value_or("");
  return detail;
}

Roe<std::string> CallControlCodec::EncodeRoster(const CallRosterDetail& detail) {
  std::vector<Value> participants;
  participants.reserve(detail.participants.size());
  for (const CallRosterEntry& entry : detail.participants) {
    participants.push_back(ObjectValue(EncodeRosterEntry(entry)));
  }
  Object json;
  json.set("call_id", detail.call_id);
  json.setJsonUInt("media_epoch", detail.media_epoch);
  json.set("participants", ArrayValue(std::move(participants)));
  return DumpJson(json);
}

Roe<CallRosterDetail> CallControlCodec::DecodeRoster(const std::string& detail_json) {
  auto json = TryParseObject(detail_json);
  auto call_id = json ? json->getString("call_id") : std::nullopt;
  if (!json || !call_id) {
    return Error("Invalid call_roster detail");
  }
  CallRosterDetail detail;
  detail.call_id = *call_id;
  detail.media_epoch = static_cast<uint32_t>(json->getNonNegInt("media_epoch").value_or(1));
  ReadParticipants(*json, detail.participants);
  return detail;
}

Roe<std::string> CallControlCodec::EncodeMediaKey(const CallMediaKeyDetail& detail) {
  Object json;
  json.set("call_id", detail.call_id);
  json.setJsonUInt("media_epoch", detail.media_epoch);
  json.set("media_key_id", detail.media_key_id);
  json.set("wrapped_key_b64", detail.wrapped_key_b64);
  return DumpJson(json);
}

Roe<CallMediaKeyDetail> CallControlCodec::DecodeMediaKey(const std::string& detail_json) {
  auto json = TryParseObject(detail_json);
  auto call_id = json ? json->getString("call_id") : std::nullopt;
  if (!json || !call_id) {
    return Error("Invalid call_media_key detail");
  }
  CallMediaKeyDetail detail;
  detail.call_id = *call_id;
  detail.media_epoch = static_cast<uint32_t>(json->getNonNegInt("media_epoch").value_or(1));
  detail.media_key_id = json->getString("media_key_id").value_or("");
  detail.wrapped_key_b64 = json->getString("wrapped_key_b64").value_or("");
  return detail;
}

Roe<std::string> CallControlCodec::EncodeEnded(const CallEndedDetail& detail) {
  Object json;
  json.set("call_id", detail.call_id);
  if (detail.duration_ms) {
    json.set("duration_ms", *detail.duration_ms);
  }
  return DumpJson(json);
}

Roe<CallEndedDetail> CallControlCodec::DecodeEnded(const std::string& detail_json) {
  auto json = TryParseObject(detail_json);
  auto call_id = json ? json->getString("call_id") : std::nullopt;
  if (!json || !call_id) {
    return Error("Invalid call_ended detail");
  }
  CallEndedDetail detail;
  detail.call_id = *call_id;
  detail.duration_ms = json->getIf<int64_t>("duration_ms");
  return detail;
}

Roe<std::string> CallControlCodec::EncodeStarted(const CallStartedDetail& detail) {
  Object json;
  json.set("call_id", detail.call_id);
  json.set("media_mode", CallMediaModeToString(detail.media_mode));
  json.set("video_allowed", detail.video_allowed);
  return DumpJson(json);
}

Roe<CallStartedDetail> CallControlCodec::DecodeStarted(const std::string& detail_json) {
  auto json = TryParseObject(detail_json);
  auto call_id = json ? json->getString("call_id") : std::nullopt;
  if (!json || !call_id) {
    return Error("Invalid call_started detail");
  }
  CallStartedDetail detail;
  detail.call_id = *call_id;
  detail.media_mode = CallMediaModeFromString(json->getString("media_mode").value_or("voice"));
  if (json->contains("video_allowed")) {
    detail.video_allowed = json->getIf<bool>("video_allowed").value_or(false);
  } else {
    detail.video_allowed = detail.media_mode == CallMediaMode::Video;
  }
  return detail;
}

Roe<std::string> CallControlCodec::EncodeSdp(const CallSdpDetail& detail) {
  Object json;
  json.set("call_id", detail.call_id);
  json.set("identity", detail.identity);
  json.set("sdp_type", detail.sdp_type);
  json.set("sdp", detail.sdp);
  return DumpJson(json);
}

Roe<CallSdpDetail> CallControlCodec::DecodeSdp(const std::string& detail_json) {
  auto json = TryParseObject(detail_json);
  auto call_id = json ? json->getString("call_id") : std::nullopt;
  if (!json || !call_id) {
    return Error("Invalid call_sdp detail");
  }
  CallSdpDetail detail;
  detail.call_id = *call_id;
  detail.identity = json->getString("identity").value_or("");
  detail.sdp_type = json->getString("sdp_type").value_or("");
  detail.sdp = json->getString("sdp").value_or("");
  if (detail.sdp_type.empty() || detail.sdp.empty()) {
    return Error("call_sdp requires sdp_type and sdp");
  }
  return detail;
}

Roe<std::string> CallControlCodec::EncodeIce(const CallIceDetail& detail) {
  Object json;
  json.set("call_id", detail.call_id);
  json.set("identity", detail.identity);
  json.set("candidate", detail.candidate);
  json.set("mid", detail.mid);
  return DumpJson(json);
}

Roe<CallIceDetail> CallControlCodec::DecodeIce(const std::string& detail_json) {
  auto json = TryParseObject(detail_json);
  auto call_id = json ? json->getString("call_id") : std::nullopt;
  if (!json || !call_id) {
    return Error("Invalid call_ice detail");
  }
  CallIceDetail detail;
  detail.call_id = *call_id;
  detail.identity = json->getString("identity").value_or("");
  detail.candidate = json->getString("candidate").value_or("");
  detail.mid = json->getString("mid").value_or("audio");
  if (detail.candidate.empty()) {
    return Error("call_ice requires candidate");
  }
  return detail;
}

Roe<std::string> CallControlCodec::EncodeSfuAttach(const CallSfuAttachDetail& detail) {
  Object json;
  json.set("call_id", detail.call_id);
  json.set("hop_peer_id", detail.hop_peer_id);
  json.set("hop_multiaddr", detail.hop_multiaddr);
  json.set("quote_id", detail.quote_id);
  json.setJsonUInt("publisher_stream_id", detail.publisher_stream_id);
  return DumpJson(json);
}

Roe<CallSfuAttachDetail> CallControlCodec::DecodeSfuAttach(const std::string& detail_json) {
  auto json = TryParseObject(detail_json);
  auto call_id = json ? json->getString("call_id") : std::nullopt;
  if (!json || !call_id) {
    return Error("Invalid call_sfu_attach detail");
  }
  CallSfuAttachDetail detail;
  detail.call_id = *call_id;
  detail.hop_peer_id = json->getString("hop_peer_id").value_or("");
  detail.hop_multiaddr = json->getString("hop_multiaddr").value_or("");
  detail.quote_id = json->getString("quote_id").value_or("");
  detail.publisher_stream_id = static_cast<uint32_t>(json->getNonNegInt("publisher_stream_id").value_or(0));
  if (detail.hop_peer_id.empty()) {
    return Error("call_sfu_attach requires hop_peer_id");
  }
  return detail;
}

Roe<std::string> CallControlCodec::EncodeSfuAttachFailed(const CallSfuAttachFailedDetail& detail) {
  std::vector<Value> prefs;
  for (const std::string& id : detail.preferred_hop_peer_ids) {
    if (!id.empty()) {
      prefs.push_back(Value(id));
    }
  }
  Object json;
  json.set("call_id", detail.call_id);
  json.set("identity", detail.identity);
  json.set("failed_hop_peer_id", detail.failed_hop_peer_id);
  json.set("error", detail.error);
  json.set("preferred_hop_peer_ids", ArrayValue(std::move(prefs)));
  return DumpJson(json);
}

Roe<CallSfuAttachFailedDetail> CallControlCodec::DecodeSfuAttachFailed(const std::string& detail_json) {
  auto json = TryParseObject(detail_json);
  auto call_id = json ? json->getString("call_id") : std::nullopt;
  if (!json || !call_id) {
    return Error("Invalid call_sfu_attach_failed detail");
  }
  CallSfuAttachFailedDetail detail;
  detail.call_id = *call_id;
  detail.identity = json->getString("identity").value_or("");
  detail.failed_hop_peer_id = json->getString("failed_hop_peer_id").value_or("");
  detail.error = json->getString("error").value_or("");
  if (const Array* prefs = json->getArray("preferred_hop_peer_ids")) {
    for (const Value& item : prefs->elements) {
      if (auto id = asString(item); id && !id->empty()) {
        detail.preferred_hop_peer_ids.push_back(*id);
      }
    }
  }
  return detail;
}

Roe<std::string> CallControlCodec::EncodeHopRefuse(const CallHopRefuseDetail& detail) {
  Object json;
  json.set("call_id", detail.call_id);
  json.set("identity", detail.identity);
  json.set("reason", detail.reason);
  json.set("message", detail.message);
  return DumpJson(json);
}

Roe<CallHopRefuseDetail> CallControlCodec::DecodeHopRefuse(const std::string& detail_json) {
  auto json = TryParseObject(detail_json);
  auto call_id = json ? json->getString("call_id") : std::nullopt;
  if (!json || !call_id) {
    return Error("Invalid call_hop_refuse detail");
  }
  CallHopRefuseDetail detail;
  detail.call_id = *call_id;
  detail.identity = json->getString("identity").value_or("");
  detail.reason = json->getString("reason").value_or("no_shared_hop");
  detail.message = json->getString("message").value_or("");
  return detail;
}

Roe<std::string> CallControlCodec::EncodeVideoRefresh(const CallVideoRefreshDetail& detail) {
  Object json;
  json.set("call_id", detail.call_id);
  json.set("identity", detail.identity);
  return DumpJson(json);
}

Roe<CallVideoRefreshDetail> CallControlCodec::DecodeVideoRefresh(const std::string& detail_json) {
  auto json = TryParseObject(detail_json);
  auto call_id = json ? json->getString("call_id") : std::nullopt;
  if (!json || !call_id) {
    return Error("Invalid call_video_refresh detail");
  }
  CallVideoRefreshDetail detail;
  detail.call_id = *call_id;
  detail.identity = json->getString("identity").value_or("");
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
  Object payload;
  payload.set("control_type", CallControlTypeToWire(type));
  payload.set("detail", detail_json);
  message.payload_json = DumpJson(payload);
  message.timestamp = util::NowUnixMs();
  message.delivery = MessageDelivery::Local;
  message.relay_visible = false;
  return message;
}

std::optional<CallControlType> CallControlCodec::ControlTypeFromMessage(const ThreadMessage& message) {
  if (message.content_type != ChatContentType::System || message.payload_json.empty()) {
    return std::nullopt;
  }
  auto json = TryParseObject(message.payload_json);
  if (!json) {
    return std::nullopt;
  }
  auto control_type = json->getString("control_type");
  if (!control_type) {
    return std::nullopt;
  }
  return CallControlTypeFromWire(*control_type);
}

bool CallControlCodec::IsCallControlMessage(const ThreadMessage& message) {
  return ControlTypeFromMessage(message).has_value();
}

} // namespace pbr
