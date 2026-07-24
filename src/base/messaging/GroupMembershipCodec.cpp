#include "base/messaging/GroupMembershipCodec.h"

#include "base/messaging/ChatPayloadTypes.h"

#include "common/Utilities.h"

#include <nlohmann/json.hpp>

namespace pbr {

namespace {

std::string HistoryModeToString(const GroupHistoryMode mode) {
  switch (mode) {
  case GroupHistoryMode::Fresh:
    return "fresh";
  case GroupHistoryMode::CopyToForkPoint:
    return "copy_to_fork_point";
  case GroupHistoryMode::UserSelected:
    return "user_selected";
  }
  return "fresh";
}

GroupHistoryMode HistoryModeFromString(const std::string& value) {
  if (value == "copy_to_fork_point") {
    return GroupHistoryMode::CopyToForkPoint;
  }
  if (value == "user_selected") {
    return GroupHistoryMode::UserSelected;
  }
  return GroupHistoryMode::Fresh;
}

std::string InvitePolicyWireToString(const GroupInvitePolicyWire policy) {
  switch (policy) {
  case GroupInvitePolicyWire::OwnerOnly:
    return "owner_only";
  case GroupInvitePolicyWire::Admins:
    return "admins";
  case GroupInvitePolicyWire::AnyMember:
    return "any_member";
  }
  return "owner_only";
}

GroupInvitePolicyWire InvitePolicyWireFromString(const std::string& value) {
  if (value == "admins") {
    return GroupInvitePolicyWire::Admins;
  }
  if (value == "any_member") {
    return GroupInvitePolicyWire::AnyMember;
  }
  return GroupInvitePolicyWire::OwnerOnly;
}

std::string HistoryVisibilityToString(const GroupHistoryVisibility visibility) {
  return visibility == GroupHistoryVisibility::SinceJoin ? "since_join" : "full";
}

GroupHistoryVisibility HistoryVisibilityFromString(const std::string& value) {
  return value == "since_join" ? GroupHistoryVisibility::SinceJoin : GroupHistoryVisibility::Full;
}

} // namespace

Roe<std::string> GroupMembershipCodec::EncodeInvite(const GroupInvitePayload& payload) {
  nlohmann::json json = {{"group_id", payload.group_id},
                         {"group_title", payload.group_title},
                         {"inviter_identity", payload.inviter_identity},
                         {"invitee_identity", payload.invitee_identity},
                         {"invite_nonce", payload.invite_nonce},
                         {"roster_epoch", payload.roster_epoch},
                         {"actor_role", MemberRoleToString(payload.actor_role)}};
  if (payload.expires_at) {
    json["expires_at"] = *payload.expires_at;
  }
  return json.dump();
}

Roe<GroupInvitePayload> GroupMembershipCodec::DecodeInvite(const std::string& detail_json) {
  const nlohmann::json json = nlohmann::json::parse(detail_json, nullptr, false);
  if (!json.is_object()) {
    return Error("Invalid group invite JSON");
  }
  GroupInvitePayload payload;
  if (!json.contains("group_id") || !json["group_id"].is_string()) {
    return Error("Missing group_id");
  }
  payload.group_id = json["group_id"].get<std::string>();
  if (json.contains("group_title") && json["group_title"].is_string()) {
    payload.group_title = json["group_title"].get<std::string>();
  }
  if (!json.contains("inviter_identity") || !json["inviter_identity"].is_string()) {
    return Error("Missing inviter_identity");
  }
  payload.inviter_identity = json["inviter_identity"].get<std::string>();
  if (!json.contains("invitee_identity") || !json["invitee_identity"].is_string()) {
    return Error("Missing invitee_identity");
  }
  payload.invitee_identity = json["invitee_identity"].get<std::string>();
  if (!json.contains("invite_nonce") || !json["invite_nonce"].is_string()) {
    return Error("Missing invite_nonce");
  }
  payload.invite_nonce = json["invite_nonce"].get<std::string>();
  if (json.contains("roster_epoch") && json["roster_epoch"].is_number_unsigned()) {
    payload.roster_epoch = json["roster_epoch"].get<uint64_t>();
  }
  if (json.contains("expires_at") && json["expires_at"].is_number_integer()) {
    payload.expires_at = json["expires_at"].get<int64_t>();
  }
  if (json.contains("actor_role") && json["actor_role"].is_string()) {
    payload.actor_role = MemberRoleFromString(json["actor_role"].get<std::string>());
  }
  return payload;
}

Roe<std::string> GroupMembershipCodec::EncodeInviteResponse(const std::string& invite_nonce,
                                                            const std::string& group_id) {
  return nlohmann::json({{"invite_nonce", invite_nonce}, {"group_id", group_id}}).dump();
}

Roe<std::pair<std::string, std::string>> GroupMembershipCodec::DecodeInviteResponse(
    const std::string& detail_json) {
  const nlohmann::json json = nlohmann::json::parse(detail_json, nullptr, false);
  if (!json.is_object() || !json.contains("invite_nonce") || !json.contains("group_id")) {
    return Error("Invalid invite response JSON");
  }
  return std::make_pair(json["invite_nonce"].get<std::string>(), json["group_id"].get<std::string>());
}

Roe<GroupMembershipCodec::InviteResponsePayload> GroupMembershipCodec::DecodeInviteResponseFromMessage(
    const ThreadMessage& message) {
  const auto control_type = ControlTypeFromMessage(message);
  if (!control_type || (*control_type != GroupMembershipControlType::GroupInviteAccept &&
                       *control_type != GroupMembershipControlType::GroupInviteDecline)) {
    return Error("Message is not a group invite response");
  }
  const nlohmann::json payload = nlohmann::json::parse(message.payload_json, nullptr, false);
  if (!payload.is_object() || !payload.contains("detail") || !payload["detail"].is_string()) {
    return Error("Missing invite response detail");
  }
  auto decoded = DecodeInviteResponse(payload["detail"].get<std::string>());
  if (!decoded) {
    return decoded.error();
  }
  InviteResponsePayload result;
  result.invite_nonce = decoded->first;
  result.group_id = decoded->second;
  result.control_type = *control_type;
  return result;
}

Roe<std::string> GroupMembershipCodec::EncodeMemberJoined(const std::string& group_id,
                                                          const std::string& member_identity, const MemberRole role,
                                                          const uint64_t roster_epoch) {
  return nlohmann::json({{"group_id", group_id},
                         {"member_identity", member_identity},
                         {"role", MemberRoleToString(role)},
                         {"roster_epoch", roster_epoch}})
      .dump();
}

Roe<std::string> GroupMembershipCodec::EncodeMemberLeft(const std::string& group_id,
                                                        const std::string& member_identity,
                                                        const uint64_t roster_epoch) {
  return nlohmann::json(
             {{"group_id", group_id}, {"member_identity", member_identity}, {"roster_epoch", roster_epoch}})
      .dump();
}

Roe<std::string> GroupMembershipCodec::EncodeMemberRemoved(const std::string& group_id,
                                                           const std::string& member_identity,
                                                           const uint64_t roster_epoch) {
  return nlohmann::json(
             {{"group_id", group_id}, {"member_identity", member_identity}, {"roster_epoch", roster_epoch}})
      .dump();
}

Roe<std::string> GroupMembershipCodec::EncodeOwnerTransferred(const std::string& group_id,
                                                            const std::string& new_owner_identity,
                                                            const uint64_t roster_epoch) {
  return nlohmann::json({{"group_id", group_id},
                         {"new_owner_identity", new_owner_identity},
                         {"roster_epoch", roster_epoch}})
      .dump();
}

Roe<std::string> GroupMembershipCodec::EncodeGroupRenamed(const std::string& group_id, const std::string& title,
                                                          const uint64_t roster_epoch) {
  return nlohmann::json({{"group_id", group_id}, {"title", title}, {"roster_epoch", roster_epoch}}).dump();
}

Roe<GroupMembershipCodec::GroupRenamedPayload> GroupMembershipCodec::DecodeGroupRenamed(
    const std::string& detail_json) {
  const nlohmann::json json = nlohmann::json::parse(detail_json, nullptr, false);
  if (!json.is_object()) {
    return Error("Invalid group_renamed detail");
  }
  GroupRenamedPayload payload;
  if (json.contains("group_id") && json["group_id"].is_string()) {
    payload.group_id = json["group_id"].get<std::string>();
  }
  if (json.contains("title") && json["title"].is_string()) {
    payload.title = json["title"].get<std::string>();
  }
  if (json.contains("roster_epoch") && json["roster_epoch"].is_number_unsigned()) {
    payload.roster_epoch = json["roster_epoch"].get<uint64_t>();
  } else if (json.contains("roster_epoch") && json["roster_epoch"].is_number_integer()) {
    payload.roster_epoch = static_cast<uint64_t>(json["roster_epoch"].get<int64_t>());
  }
  if (payload.group_id.empty() || payload.title.empty()) {
    return Error("group_renamed missing group_id or title");
  }
  return payload;
}

Roe<GroupMembershipCodec::GroupRenamedPayload> GroupMembershipCodec::DecodeGroupRenamedFromMessage(
    const ThreadMessage& message) {
  const auto control_type = ControlTypeFromMessage(message);
  if (!control_type || *control_type != GroupMembershipControlType::GroupRenamed) {
    return Error("Message is not a group rename");
  }
  const nlohmann::json payload = nlohmann::json::parse(message.payload_json, nullptr, false);
  if (!payload.is_object() || !payload.contains("detail") || !payload["detail"].is_string()) {
    return Error("Missing group_renamed detail");
  }
  return DecodeGroupRenamed(payload["detail"].get<std::string>());
}

Roe<std::string> GroupMembershipCodec::EncodeGroupForked(const GroupForkPayload& payload) {
  nlohmann::json json = {{"source_group_id", payload.source_group_id},
                         {"new_group_id", payload.new_group_id},
                         {"new_group_title", payload.new_group_title},
                         {"selected_identities", payload.selected_identities},
                         {"history_mode", HistoryModeToString(payload.history_mode)},
                         {"actor_identity", payload.actor_identity},
                         {"roster_epoch", payload.roster_epoch}};
  if (payload.fork_message_id) {
    json["fork_message_id"] = *payload.fork_message_id;
  }
  return json.dump();
}

std::vector<TranscriptChatAction> GroupMembershipCodec::BuildInviteChatActions(const GroupInvitePayload& invite) {
  std::vector<TranscriptChatAction> actions;
  actions.push_back({.label = "Accept",
                     .message = "Accept group invitation",
                     .payload = nlohmann::json({{"type", "accept_group_invite"}, {"invite_nonce", invite.invite_nonce}})
                                     .dump()});
  actions.push_back({.label = "Decline",
                     .message = "Decline group invitation",
                     .payload = nlohmann::json({{"type", "decline_group_invite"}, {"invite_nonce", invite.invite_nonce}})
                                     .dump()});
  actions.push_back(
      {.label = "Block",
       .message = "Block inviter",
       .payload = nlohmann::json({{"type", "block_group_inviter"},
                                  {"inviter_identity", invite.inviter_identity},
                                  {"invite_nonce", invite.invite_nonce}})
                       .dump()});
  return actions;
}

std::optional<GroupMembershipControlType> GroupMembershipCodec::ControlTypeFromMessage(const ThreadMessage& message) {
  if (message.content_type != ChatContentType::System || message.payload_json.empty()) {
    return std::nullopt;
  }
  const nlohmann::json payload = nlohmann::json::parse(message.payload_json, nullptr, false);
  if (!payload.is_object() || !payload.contains("control_type") || !payload["control_type"].is_string()) {
    return std::nullopt;
  }
  return GroupMembershipControlTypeFromWire(payload["control_type"].get<std::string>());
}

Roe<GroupInvitePayload> GroupMembershipCodec::DecodeInviteFromMessage(const ThreadMessage& message) {
  const auto control_type = ControlTypeFromMessage(message);
  if (!control_type || *control_type != GroupMembershipControlType::GroupInvite) {
    return Error("Message is not a group invite");
  }
  const nlohmann::json payload = nlohmann::json::parse(message.payload_json, nullptr, false);
  if (!payload.is_object() || !payload.contains("detail") || !payload["detail"].is_string()) {
    return Error("Missing invite detail");
  }
  return DecodeInvite(payload["detail"].get<std::string>());
}

Roe<ThreadMessage> GroupMembershipCodec::BuildSystemMessage(const std::string& thread_id,
                                                            const GroupMembershipControlType type,
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
      nlohmann::json({{"control_type", GroupMembershipControlTypeToWire(type)}, {"detail", detail_json}}).dump();
  message.timestamp = util::NowUnixMs();
  message.delivery = MessageDelivery::Local;
  message.relay_visible = false;
  return message;
}

Roe<GroupPolicy> GroupMembershipCodec::DecodeGroupPolicy(const std::string& policy_json) {
  const nlohmann::json json = nlohmann::json::parse(policy_json, nullptr, false);
  if (!json.is_object()) {
    return Error("Invalid group policy JSON");
  }
  GroupPolicy policy;
  if (json.contains("invite_policy") && json["invite_policy"].is_string()) {
    policy.invite_policy = InvitePolicyWireFromString(json["invite_policy"].get<std::string>());
  }
  if (json.contains("history_visibility") && json["history_visibility"].is_string()) {
    policy.history_visibility = HistoryVisibilityFromString(json["history_visibility"].get<std::string>());
  }
  return policy;
}

std::string GroupMembershipCodec::EncodeGroupPolicy(const GroupPolicy& policy) {
  return nlohmann::json({{"invite_policy", InvitePolicyWireToString(policy.invite_policy)},
                         {"history_visibility", HistoryVisibilityToString(policy.history_visibility)}})
      .dump();
}

} // namespace pbr
