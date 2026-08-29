#include "base/messaging/GroupMembershipCodec.h"

#include "base/messaging/ChatPayloadTypes.h"
#include "common/Utilities.h"
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

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

uint64_t ReadRosterEpoch(const Object& json) {
  return json.getNonNegInt("roster_epoch").value_or(0);
}

Roe<std::string> DetailFromMessage(const ThreadMessage& message, const GroupMembershipControlType expected,
                                   const char* not_type_error) {
  const auto control_type = GroupMembershipCodec::ControlTypeFromMessage(message);
  if (!control_type || *control_type != expected) {
    return Error(not_type_error);
  }
  auto payload = TryParseObject(message.payload_json);
  if (!payload) {
    return Error("Missing membership detail");
  }
  auto detail = payload->getString("detail");
  if (!detail) {
    return Error("Missing membership detail");
  }
  return *detail;
}

Object MakeStringObject(std::initializer_list<std::pair<const char*, std::string>> fields) {
  Object object;
  for (const auto& [key, value] : fields) {
    object.set(key, value);
  }
  return object;
}

} // namespace

Roe<std::string> GroupMembershipCodec::EncodeInvite(const GroupInvitePayload& payload) {
  Object json;
  json.set("group_id", payload.group_id);
  json.set("group_title", payload.group_title);
  json.set("inviter_identity", payload.inviter_identity);
  json.set("invitee_identity", payload.invitee_identity);
  json.set("invite_nonce", payload.invite_nonce);
  json.setJsonUInt("roster_epoch", payload.roster_epoch);
  json.set("actor_role", MemberRoleToString(payload.actor_role));
  if (payload.expires_at) {
    json.set("expires_at", *payload.expires_at);
  }
  return DumpJson(json);
}

Roe<GroupInvitePayload> GroupMembershipCodec::DecodeInvite(const std::string& detail_json) {
  auto json = TryParseObject(detail_json);
  if (!json) {
    return Error("Invalid group invite JSON");
  }
  GroupInvitePayload payload;
  auto group_id = json->getString("group_id");
  if (!group_id) {
    return Error("Missing group_id");
  }
  payload.group_id = *group_id;
  if (auto title = json->getString("group_title")) {
    payload.group_title = *title;
  }
  auto inviter = json->getString("inviter_identity");
  if (!inviter) {
    return Error("Missing inviter_identity");
  }
  payload.inviter_identity = *inviter;
  auto invitee = json->getString("invitee_identity");
  if (!invitee) {
    return Error("Missing invitee_identity");
  }
  payload.invitee_identity = *invitee;
  auto nonce = json->getString("invite_nonce");
  if (!nonce) {
    return Error("Missing invite_nonce");
  }
  payload.invite_nonce = *nonce;
  if (auto epoch = json->getNonNegInt("roster_epoch")) {
    payload.roster_epoch = *epoch;
  }
  if (auto expires = json->getIf<int64_t>("expires_at")) {
    payload.expires_at = *expires;
  }
  if (auto role = json->getString("actor_role")) {
    payload.actor_role = MemberRoleFromString(*role);
  }
  return payload;
}

Roe<std::string> GroupMembershipCodec::EncodeInviteResponse(const std::string& invite_nonce,
                                                            const std::string& group_id) {
  return DumpJson(MakeStringObject({{"invite_nonce", invite_nonce}, {"group_id", group_id}}));
}

Roe<std::pair<std::string, std::string>> GroupMembershipCodec::DecodeInviteResponse(
    const std::string& detail_json) {
  auto json = TryParseObject(detail_json);
  if (!json) {
    return Error("Invalid invite response JSON");
  }
  auto nonce = json->getString("invite_nonce");
  auto group_id = json->getString("group_id");
  if (!nonce || !group_id) {
    return Error("Invalid invite response JSON");
  }
  return std::make_pair(*nonce, *group_id);
}

Roe<GroupMembershipCodec::InviteResponsePayload> GroupMembershipCodec::DecodeInviteResponseFromMessage(
    const ThreadMessage& message) {
  const auto control_type = ControlTypeFromMessage(message);
  if (!control_type || (*control_type != GroupMembershipControlType::GroupInviteAccept &&
                       *control_type != GroupMembershipControlType::GroupInviteDecline)) {
    return Error("Message is not a group invite response");
  }
  auto payload = TryParseObject(message.payload_json);
  if (!payload) {
    return Error("Missing invite response detail");
  }
  auto detail = payload->getString("detail");
  if (!detail) {
    return Error("Missing invite response detail");
  }
  auto decoded = DecodeInviteResponse(*detail);
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
  Object json;
  json.set("group_id", payload.group_id);
  json.set("member_identity", payload.member_identity);
  json.set("role", MemberRoleToString(payload.role));
  json.setJsonUInt("roster_epoch", payload.roster_epoch);
  if (!payload.members.empty()) {
    std::vector<Value> members;
    members.reserve(payload.members.size());
    for (const MemberJoinedEntry& entry : payload.members) {
      Object row;
      row.set("member_identity", entry.member_identity);
      row.set("role", MemberRoleToString(entry.role));
      members.push_back(ObjectValue(std::move(row)));
    }
    json.set("members", ArrayValue(std::move(members)));
  }
  return DumpJson(json);
}

Roe<std::string> GroupMembershipCodec::EncodeMemberLeft(const std::string& group_id,
                                                        const std::string& member_identity,
                                                        const uint64_t roster_epoch) {
  Object json;
  json.set("group_id", group_id);
  json.set("member_identity", member_identity);
  json.setJsonUInt("roster_epoch", roster_epoch);
  return DumpJson(json);
}

Roe<std::string> GroupMembershipCodec::EncodeMemberRemoved(const std::string& group_id,
                                                           const std::string& member_identity,
                                                           const uint64_t roster_epoch) {
  Object json;
  json.set("group_id", group_id);
  json.set("member_identity", member_identity);
  json.setJsonUInt("roster_epoch", roster_epoch);
  return DumpJson(json);
}

Roe<std::string> GroupMembershipCodec::EncodeOwnerTransferred(const std::string& group_id,
                                                            const std::string& new_owner_identity,
                                                            const uint64_t roster_epoch,
                                                            const bool leave_previous) {
  Object json;
  json.set("group_id", group_id);
  json.set("new_owner_identity", new_owner_identity);
  json.setJsonUInt("roster_epoch", roster_epoch);
  json.set("leave_previous", leave_previous);
  return DumpJson(json);
}

Roe<GroupMembershipCodec::MemberJoinedPayload> GroupMembershipCodec::DecodeMemberJoined(
    const std::string& detail_json) {
  auto json = TryParseObject(detail_json);
  if (!json) {
    return Error("Invalid member_joined detail");
  }
  MemberJoinedPayload payload;
  if (auto group_id = json->getString("group_id")) {
    payload.group_id = *group_id;
  }
  if (auto member = json->getString("member_identity")) {
    payload.member_identity = *member;
  }
  if (auto role = json->getString("role")) {
    payload.role = MemberRoleFromString(*role);
  }
  payload.roster_epoch = ReadRosterEpoch(*json);
  if (const Array* members = json->getArray("members")) {
    for (const Value& row_value : members->elements) {
      const Object* row = asObject(row_value);
      if (!row) {
        continue;
      }
      auto member = row->getString("member_identity");
      if (!member) {
        continue;
      }
      MemberJoinedEntry entry;
      entry.member_identity = *member;
      if (auto role = row->getString("role")) {
        entry.role = MemberRoleFromString(*role);
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
  auto json = TryParseObject(detail_json);
  if (!json) {
    return Error("Invalid member_left detail");
  }
  MemberLeftPayload payload;
  if (auto group_id = json->getString("group_id")) {
    payload.group_id = *group_id;
  }
  if (auto member = json->getString("member_identity")) {
    payload.member_identity = *member;
  }
  payload.roster_epoch = ReadRosterEpoch(*json);
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
  auto json = TryParseObject(detail_json);
  if (!json) {
    return Error("Invalid member_removed detail");
  }
  MemberRemovedPayload payload;
  if (auto group_id = json->getString("group_id")) {
    payload.group_id = *group_id;
  }
  if (auto member = json->getString("member_identity")) {
    payload.member_identity = *member;
  }
  payload.roster_epoch = ReadRosterEpoch(*json);
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
  auto json = TryParseObject(detail_json);
  if (!json) {
    return Error("Invalid owner_transferred detail");
  }
  OwnerTransferredPayload payload;
  if (auto group_id = json->getString("group_id")) {
    payload.group_id = *group_id;
  }
  if (auto owner = json->getString("new_owner_identity")) {
    payload.new_owner_identity = *owner;
  }
  payload.roster_epoch = ReadRosterEpoch(*json);
  if (auto leave = json->getIf<bool>("leave_previous")) {
    payload.leave_previous = *leave;
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
  Object json;
  json.set("group_id", group_id);
  json.set("title", title);
  json.setJsonUInt("roster_epoch", roster_epoch);
  return DumpJson(json);
}

Roe<GroupMembershipCodec::GroupRenamedPayload> GroupMembershipCodec::DecodeGroupRenamed(
    const std::string& detail_json) {
  auto json = TryParseObject(detail_json);
  if (!json) {
    return Error("Invalid group_renamed detail");
  }
  GroupRenamedPayload payload;
  if (auto group_id = json->getString("group_id")) {
    payload.group_id = *group_id;
  }
  if (auto title = json->getString("title")) {
    payload.title = *title;
  }
  payload.roster_epoch = ReadRosterEpoch(*json);
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
  auto payload = TryParseObject(message.payload_json);
  if (!payload) {
    return Error("Missing group_renamed detail");
  }
  auto detail = payload->getString("detail");
  if (!detail) {
    return Error("Missing group_renamed detail");
  }
  return DecodeGroupRenamed(*detail);
}

Roe<std::string> GroupMembershipCodec::EncodeGroupForked(const GroupForkPayload& payload) {
  Object json;
  json.set("source_group_id", payload.source_group_id);
  json.set("new_group_id", payload.new_group_id);
  json.set("new_group_title", payload.new_group_title);
  std::vector<Value> identities;
  identities.reserve(payload.selected_identities.size());
  for (const std::string& identity : payload.selected_identities) {
    identities.push_back(Value(identity));
  }
  json.set("selected_identities", ArrayValue(std::move(identities)));
  json.set("history_mode", HistoryModeToString(payload.history_mode));
  json.set("actor_identity", payload.actor_identity);
  json.setJsonUInt("roster_epoch", payload.roster_epoch);
  if (payload.fork_message_id) {
    json.set("fork_message_id", *payload.fork_message_id);
  }
  return DumpJson(json);
}

std::vector<TranscriptChatAction> GroupMembershipCodec::BuildInviteChatActions(const GroupInvitePayload& invite) {
  std::vector<TranscriptChatAction> actions;
  {
    Object payload;
    payload.set("type", "accept_group_invite");
    payload.set("invite_nonce", invite.invite_nonce);
    actions.push_back({.label = "Accept", .message = "Accept group invitation", .payload = DumpJson(payload)});
  }
  {
    Object payload;
    payload.set("type", "decline_group_invite");
    payload.set("invite_nonce", invite.invite_nonce);
    actions.push_back({.label = "Decline", .message = "Decline group invitation", .payload = DumpJson(payload)});
  }
  {
    Object payload;
    payload.set("type", "block_group_inviter");
    payload.set("inviter_identity", invite.inviter_identity);
    payload.set("invite_nonce", invite.invite_nonce);
    actions.push_back({.label = "Block", .message = "Block inviter", .payload = DumpJson(payload)});
  }
  return actions;
}

std::vector<TranscriptChatAction> GroupMembershipCodec::BuildOwnerUnreachableChatActions(
    const std::string& group_id, const std::string& owner_identity) {
  std::vector<TranscriptChatAction> actions;
  {
    Object payload;
    payload.set("type", "fork_group");
    payload.set("group_id", group_id);
    actions.push_back({.label = "Start a new group", .message = "Fork this group", .payload = DumpJson(payload)});
  }
  {
    Object payload;
    payload.set("type", "message_group_owner");
    payload.set("owner_identity", owner_identity);
    actions.push_back(
        {.label = "Message owner", .message = "Open a chat with the owner", .payload = DumpJson(payload)});
  }
  {
    Object payload;
    payload.set("type", "dismiss_owner_advisory");
    payload.set("group_id", group_id);
    actions.push_back({.label = "Got it", .message = "Dismiss this note", .payload = DumpJson(payload)});
  }
  return actions;
}

bool GroupMembershipCodec::IsOwnerUnreachableAdvisory(const ThreadMessage& message) {
  const auto control_type = ControlTypeFromMessage(message);
  return control_type && *control_type == GroupMembershipControlType::GroupOwnerUnreachable;
}

void GroupMembershipCodec::ApplyOwnerUnreachableResolution(ThreadMessage& message) {
  Object payload = TryParseObject(message.payload_json).value_or(Object{});
  payload.set("control_type", GroupMembershipControlTypeToWire(GroupMembershipControlType::GroupOwnerUnreachable));
  payload.set("resolution", "dismissed");
  message.payload_json = DumpJson(payload);
  message.chat_actions.clear();
  message.content_rml.reset();
}

bool GroupMembershipCodec::IsOwnerUnreachableResolved(const ThreadMessage& message) {
  if (message.payload_json.empty()) {
    return false;
  }
  auto payload = TryParseObject(message.payload_json);
  if (!payload) {
    return false;
  }
  auto resolution = payload->getString("resolution");
  return resolution && *resolution == "dismissed";
}

void GroupMembershipCodec::ApplyInviteResolution(ThreadMessage& message, const InviteStatus status,
                                                 const std::string& status_text) {
  Object payload = TryParseObject(message.payload_json).value_or(Object{});
  payload.set("control_type", GroupMembershipControlTypeToWire(GroupMembershipControlType::GroupInvite));
  switch (status) {
  case InviteStatus::Accepted:
    payload.set("resolution", "accepted");
    break;
  case InviteStatus::Declined:
    payload.set("resolution", "declined");
    break;
  case InviteStatus::Blocked:
    payload.set("resolution", "blocked");
    break;
  default:
    payload.set("resolution", "resolved");
    break;
  }
  message.payload_json = DumpJson(payload);
  message.text = status_text;
  message.chat_actions.clear();
  message.content_rml.reset();
}

std::optional<InviteStatus> GroupMembershipCodec::InviteResolutionFromMessage(const ThreadMessage& message) {
  if (message.payload_json.empty()) {
    return std::nullopt;
  }
  auto payload = TryParseObject(message.payload_json);
  if (!payload) {
    return std::nullopt;
  }
  auto value = payload->getString("resolution");
  if (!value) {
    return std::nullopt;
  }
  if (*value == "accepted") {
    return InviteStatus::Accepted;
  }
  if (*value == "declined") {
    return InviteStatus::Declined;
  }
  if (*value == "blocked") {
    return InviteStatus::Blocked;
  }
  return InviteStatus::Expired;
}

std::optional<GroupMembershipControlType> GroupMembershipCodec::ControlTypeFromMessage(const ThreadMessage& message) {
  if (message.content_type != ChatContentType::System || message.payload_json.empty()) {
    return std::nullopt;
  }
  auto payload = TryParseObject(message.payload_json);
  if (!payload) {
    return std::nullopt;
  }
  auto control_type = payload->getString("control_type");
  if (!control_type) {
    return std::nullopt;
  }
  return GroupMembershipControlTypeFromWire(*control_type);
}

Roe<GroupInvitePayload> GroupMembershipCodec::DecodeInviteFromMessage(const ThreadMessage& message) {
  const auto control_type = ControlTypeFromMessage(message);
  if (!control_type || *control_type != GroupMembershipControlType::GroupInvite) {
    return Error("Message is not a group invite");
  }
  auto payload = TryParseObject(message.payload_json);
  if (!payload) {
    return Error("Missing invite detail");
  }
  auto detail = payload->getString("detail");
  if (!detail) {
    return Error("Missing invite detail");
  }
  return DecodeInvite(*detail);
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
  Object payload;
  payload.set("control_type", GroupMembershipControlTypeToWire(type));
  payload.set("detail", detail_json);
  message.payload_json = DumpJson(payload);
  message.timestamp = util::NowUnixMs();
  message.delivery = MessageDelivery::Local;
  message.relay_visible = false;
  return message;
}

Roe<GroupPolicy> GroupMembershipCodec::DecodeGroupPolicy(const std::string& policy_json) {
  auto json = TryParseObject(policy_json);
  if (!json) {
    return Error("Invalid group policy JSON");
  }
  GroupPolicy policy;
  if (auto invite_policy = json->getString("invite_policy")) {
    policy.invite_policy = InvitePolicyWireFromString(*invite_policy);
  }
  if (auto history = json->getString("history_visibility")) {
    policy.history_visibility = HistoryVisibilityFromString(*history);
  }
  return policy;
}

std::string GroupMembershipCodec::EncodeGroupPolicy(const GroupPolicy& policy) {
  Object json;
  json.set("invite_policy", InvitePolicyWireToString(policy.invite_policy));
  json.set("history_visibility", HistoryVisibilityToString(policy.history_visibility));
  return DumpJson(json);
}

} // namespace pbr
