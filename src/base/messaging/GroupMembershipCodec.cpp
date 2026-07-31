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
  MemberJoinedPayload payload;
  payload.group_id = group_id;
  payload.member_identity = member_identity;
  payload.role = role;
  payload.roster_epoch = roster_epoch;
  return EncodeMemberJoined(payload);
}

Roe<std::string> GroupMembershipCodec::EncodeMemberJoined(const MemberJoinedPayload& payload) {
  nlohmann::json json{{"group_id", payload.group_id},
                      {"member_identity", payload.member_identity},
                      {"role", MemberRoleToString(payload.role)},
                      {"roster_epoch", payload.roster_epoch}};
  if (!payload.members.empty()) {
    nlohmann::json members = nlohmann::json::array();
    for (const MemberJoinedEntry& entry : payload.members) {
      members.push_back(nlohmann::json{{"member_identity", entry.member_identity},
                                       {"role", MemberRoleToString(entry.role)}});
    }
    json["members"] = std::move(members);
  }
  return json.dump();
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
                                                            const uint64_t roster_epoch,
                                                            const bool leave_previous) {
  return nlohmann::json({{"group_id", group_id},
                         {"new_owner_identity", new_owner_identity},
                         {"roster_epoch", roster_epoch},
                         {"leave_previous", leave_previous}})
      .dump();
}

namespace {

uint64_t ReadRosterEpoch(const nlohmann::json& json) {
  if (json.contains("roster_epoch") && json["roster_epoch"].is_number_unsigned()) {
    return json["roster_epoch"].get<uint64_t>();
  }
  if (json.contains("roster_epoch") && json["roster_epoch"].is_number_integer()) {
    return static_cast<uint64_t>(json["roster_epoch"].get<int64_t>());
  }
  return 0;
}

Roe<std::string> DetailFromMessage(const ThreadMessage& message, const GroupMembershipControlType expected,
                                   const char* not_type_error) {
  const auto control_type = GroupMembershipCodec::ControlTypeFromMessage(message);
  if (!control_type || *control_type != expected) {
    return Error(not_type_error);
  }
  const nlohmann::json payload = nlohmann::json::parse(message.payload_json, nullptr, false);
  if (!payload.is_object() || !payload.contains("detail") || !payload["detail"].is_string()) {
    return Error("Missing membership detail");
  }
  return payload["detail"].get<std::string>();
}

} // namespace

Roe<GroupMembershipCodec::MemberJoinedPayload> GroupMembershipCodec::DecodeMemberJoined(
    const std::string& detail_json) {
  const nlohmann::json json = nlohmann::json::parse(detail_json, nullptr, false);
  if (!json.is_object()) {
    return Error("Invalid member_joined detail");
  }
  MemberJoinedPayload payload;
  if (json.contains("group_id") && json["group_id"].is_string()) {
    payload.group_id = json["group_id"].get<std::string>();
  }
  if (json.contains("member_identity") && json["member_identity"].is_string()) {
    payload.member_identity = json["member_identity"].get<std::string>();
  }
  if (json.contains("role") && json["role"].is_string()) {
    payload.role = MemberRoleFromString(json["role"].get<std::string>());
  }
  payload.roster_epoch = ReadRosterEpoch(json);
  if (json.contains("members") && json["members"].is_array()) {
    for (const nlohmann::json& row : json["members"]) {
      if (!row.is_object() || !row.contains("member_identity") || !row["member_identity"].is_string()) {
        continue;
      }
      MemberJoinedEntry entry;
      entry.member_identity = row["member_identity"].get<std::string>();
      if (row.contains("role") && row["role"].is_string()) {
        entry.role = MemberRoleFromString(row["role"].get<std::string>());
      }
      payload.members.push_back(std::move(entry));
    }
  }
  if (payload.group_id.empty() || payload.member_identity.empty()) {
    return Error("member_joined missing group_id or member_identity");
  }
  return payload;
}

Roe<GroupMembershipCodec::MemberJoinedPayload> GroupMembershipCodec::DecodeMemberJoinedFromMessage(
    const ThreadMessage& message) {
  auto detail =
      DetailFromMessage(message, GroupMembershipControlType::MemberJoined, "Message is not a member_joined");
  if (!detail) {
    return detail.error();
  }
  return DecodeMemberJoined(*detail);
}

Roe<GroupMembershipCodec::MemberLeftPayload> GroupMembershipCodec::DecodeMemberLeft(
    const std::string& detail_json) {
  const nlohmann::json json = nlohmann::json::parse(detail_json, nullptr, false);
  if (!json.is_object()) {
    return Error("Invalid member_left detail");
  }
  MemberLeftPayload payload;
  if (json.contains("group_id") && json["group_id"].is_string()) {
    payload.group_id = json["group_id"].get<std::string>();
  }
  if (json.contains("member_identity") && json["member_identity"].is_string()) {
    payload.member_identity = json["member_identity"].get<std::string>();
  }
  payload.roster_epoch = ReadRosterEpoch(json);
  if (payload.group_id.empty() || payload.member_identity.empty()) {
    return Error("member_left missing group_id or member_identity");
  }
  return payload;
}

Roe<GroupMembershipCodec::MemberLeftPayload> GroupMembershipCodec::DecodeMemberLeftFromMessage(
    const ThreadMessage& message) {
  auto detail = DetailFromMessage(message, GroupMembershipControlType::MemberLeft, "Message is not a member_left");
  if (!detail) {
    return detail.error();
  }
  return DecodeMemberLeft(*detail);
}

Roe<GroupMembershipCodec::MemberRemovedPayload> GroupMembershipCodec::DecodeMemberRemoved(
    const std::string& detail_json) {
  const nlohmann::json json = nlohmann::json::parse(detail_json, nullptr, false);
  if (!json.is_object()) {
    return Error("Invalid member_removed detail");
  }
  MemberRemovedPayload payload;
  if (json.contains("group_id") && json["group_id"].is_string()) {
    payload.group_id = json["group_id"].get<std::string>();
  }
  if (json.contains("member_identity") && json["member_identity"].is_string()) {
    payload.member_identity = json["member_identity"].get<std::string>();
  }
  payload.roster_epoch = ReadRosterEpoch(json);
  if (payload.group_id.empty() || payload.member_identity.empty()) {
    return Error("member_removed missing group_id or member_identity");
  }
  return payload;
}

Roe<GroupMembershipCodec::MemberRemovedPayload> GroupMembershipCodec::DecodeMemberRemovedFromMessage(
    const ThreadMessage& message) {
  auto detail =
      DetailFromMessage(message, GroupMembershipControlType::MemberRemoved, "Message is not a member_removed");
  if (!detail) {
    return detail.error();
  }
  return DecodeMemberRemoved(*detail);
}

Roe<GroupMembershipCodec::OwnerTransferredPayload> GroupMembershipCodec::DecodeOwnerTransferred(
    const std::string& detail_json) {
  const nlohmann::json json = nlohmann::json::parse(detail_json, nullptr, false);
  if (!json.is_object()) {
    return Error("Invalid owner_transferred detail");
  }
  OwnerTransferredPayload payload;
  if (json.contains("group_id") && json["group_id"].is_string()) {
    payload.group_id = json["group_id"].get<std::string>();
  }
  if (json.contains("new_owner_identity") && json["new_owner_identity"].is_string()) {
    payload.new_owner_identity = json["new_owner_identity"].get<std::string>();
  }
  payload.roster_epoch = ReadRosterEpoch(json);
  if (json.contains("leave_previous") && json["leave_previous"].is_boolean()) {
    payload.leave_previous = json["leave_previous"].get<bool>();
  }
  if (payload.group_id.empty() || payload.new_owner_identity.empty()) {
    return Error("owner_transferred missing group_id or new_owner_identity");
  }
  return payload;
}

Roe<GroupMembershipCodec::OwnerTransferredPayload> GroupMembershipCodec::DecodeOwnerTransferredFromMessage(
    const ThreadMessage& message) {
  auto detail = DetailFromMessage(message, GroupMembershipControlType::OwnerTransferred,
                                  "Message is not an owner_transferred");
  if (!detail) {
    return detail.error();
  }
  return DecodeOwnerTransferred(*detail);
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

std::vector<TranscriptChatAction> GroupMembershipCodec::BuildOwnerUnreachableChatActions(
    const std::string& group_id, const std::string& owner_identity) {
  std::vector<TranscriptChatAction> actions;
  actions.push_back({.label = "Start a new group",
                     .message = "Fork this group",
                     .payload = nlohmann::json({{"type", "fork_group"}, {"group_id", group_id}}).dump()});
  actions.push_back({.label = "Message owner",
                     .message = "Open a chat with the owner",
                     .payload =
                         nlohmann::json({{"type", "message_group_owner"}, {"owner_identity", owner_identity}}).dump()});
  actions.push_back(
      {.label = "Got it",
       .message = "Dismiss this note",
       .payload = nlohmann::json({{"type", "dismiss_owner_advisory"}, {"group_id", group_id}}).dump()});
  return actions;
}

bool GroupMembershipCodec::IsOwnerUnreachableAdvisory(const ThreadMessage& message) {
  const auto control_type = ControlTypeFromMessage(message);
  return control_type && *control_type == GroupMembershipControlType::GroupOwnerUnreachable;
}

void GroupMembershipCodec::ApplyOwnerUnreachableResolution(ThreadMessage& message) {
  nlohmann::json payload = nlohmann::json::parse(message.payload_json, nullptr, false);
  if (!payload.is_object()) {
    payload = nlohmann::json::object();
  }
  payload["control_type"] = GroupMembershipControlTypeToWire(GroupMembershipControlType::GroupOwnerUnreachable);
  payload["resolution"] = "dismissed";
  message.payload_json = payload.dump();
  message.chat_actions.clear();
  message.content_rml.reset();
}

bool GroupMembershipCodec::IsOwnerUnreachableResolved(const ThreadMessage& message) {
  if (message.payload_json.empty()) {
    return false;
  }
  const nlohmann::json payload = nlohmann::json::parse(message.payload_json, nullptr, false);
  return payload.is_object() && payload.contains("resolution") && payload["resolution"].is_string() &&
         payload["resolution"].get<std::string>() == "dismissed";
}

void GroupMembershipCodec::ApplyInviteResolution(ThreadMessage& message, const InviteStatus status,
                                                 const std::string& status_text) {
  nlohmann::json payload = nlohmann::json::parse(message.payload_json, nullptr, false);
  if (!payload.is_object()) {
    payload = nlohmann::json::object();
  }
  payload["control_type"] = GroupMembershipControlTypeToWire(GroupMembershipControlType::GroupInvite);
  switch (status) {
  case InviteStatus::Accepted:
    payload["resolution"] = "accepted";
    break;
  case InviteStatus::Declined:
    payload["resolution"] = "declined";
    break;
  case InviteStatus::Blocked:
    payload["resolution"] = "blocked";
    break;
  default:
    payload["resolution"] = "resolved";
    break;
  }
  message.payload_json = payload.dump();
  message.text = status_text;
  message.chat_actions.clear();
  message.content_rml.reset();
}

std::optional<InviteStatus> GroupMembershipCodec::InviteResolutionFromMessage(const ThreadMessage& message) {
  if (message.payload_json.empty()) {
    return std::nullopt;
  }
  const nlohmann::json payload = nlohmann::json::parse(message.payload_json, nullptr, false);
  if (!payload.is_object() || !payload.contains("resolution") || !payload["resolution"].is_string()) {
    return std::nullopt;
  }
  const std::string value = payload["resolution"].get<std::string>();
  if (value == "accepted") {
    return InviteStatus::Accepted;
  }
  if (value == "declined") {
    return InviteStatus::Declined;
  }
  if (value == "blocked") {
    return InviteStatus::Blocked;
  }
  return InviteStatus::Expired;
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
