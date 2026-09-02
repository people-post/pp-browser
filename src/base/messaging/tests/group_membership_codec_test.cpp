#include "base/messaging/GroupMembershipCodec.h"
#include "base/messaging/GroupE2ePayloadCodec.h"
#include "base/messaging/GroupTypes.h"
#include "common/thread/ThreadTypes.h"
#include "common/ValueJson.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

std::string ControlPayload(const std::string& control_type, const std::string& detail) {
  Object payload;
  payload.set("control_type", control_type);
  payload.set("detail", detail);
  return DumpJson(payload);
}

TEST(GroupMembershipCodecTest, InviteRoundTrip) {
  GroupInvitePayload payload;
  payload.group_id = "group:test";
  payload.group_title = "Test Group";
  payload.inviter_identity = "relay:alice";
  payload.invitee_identity = "relay:bob";
  payload.invite_nonce = "nonce-1";
  payload.roster_epoch = 2;
  payload.actor_role = MemberRole::Owner;

  auto encoded = GroupMembershipCodec::EncodeInvite(payload);
  ASSERT_TRUE(encoded);
  auto decoded = GroupMembershipCodec::DecodeInvite(*encoded);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->group_id, payload.group_id);
  EXPECT_EQ(decoded->invitee_identity, payload.invitee_identity);
  EXPECT_EQ(decoded->roster_epoch, payload.roster_epoch);
}

TEST(GroupMembershipCodecTest, GroupPolicyRoundTrip) {
  GroupPolicy policy;
  policy.invite_policy = GroupInvitePolicyWire::OwnerOnly;
  policy.history_visibility = GroupHistoryVisibility::Full;
  const std::string json = GroupMembershipCodec::EncodeGroupPolicy(policy);
  auto decoded = GroupMembershipCodec::DecodeGroupPolicy(json);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->invite_policy, GroupInvitePolicyWire::OwnerOnly);
}

TEST(GroupMembershipCodecTest, BuildInviteChatActions) {
  GroupInvitePayload payload;
  payload.invite_nonce = "nonce-abc";
  payload.inviter_identity = "relay:alice";
  const auto actions = GroupMembershipCodec::BuildInviteChatActions(payload);
  ASSERT_EQ(actions.size(), 3u);
  EXPECT_EQ(actions[0].label, "Accept");
  EXPECT_TRUE(actions[2].payload.has_value());
  EXPECT_NE(actions[2].payload->find("block_group_inviter"), std::string::npos);
}

TEST(GroupMembershipCodecTest, InviteResponseRoundTrip) {
  auto encoded = GroupMembershipCodec::EncodeInviteResponse("nonce-1", "group:test");
  ASSERT_TRUE(encoded);
  auto decoded = GroupMembershipCodec::DecodeInviteResponse(*encoded);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->first, "nonce-1");
  EXPECT_EQ(decoded->second, "group:test");
}

TEST(GroupMembershipCodecTest, DecodeInviteResponseFromMessage) {
  auto detail = GroupMembershipCodec::EncodeInviteResponse("nonce-xyz", "group:abc");
  ASSERT_TRUE(detail);
  ThreadMessage message;
  message.content_type = ChatContentType::System;
  message.payload_json = ControlPayload("group_invite_accept", *detail);
  auto decoded = GroupMembershipCodec::DecodeInviteResponseFromMessage(message);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->invite_nonce, "nonce-xyz");
  EXPECT_EQ(decoded->group_id, "group:abc");
  EXPECT_EQ(decoded->control_type, GroupMembershipControlType::GroupInviteAccept);

  message.payload_json = ControlPayload("group_invite_decline", *detail);
  auto declined = GroupMembershipCodec::DecodeInviteResponseFromMessage(message);
  ASSERT_TRUE(declined);
  EXPECT_EQ(declined->control_type, GroupMembershipControlType::GroupInviteDecline);
}

TEST(GroupMembershipCodecTest, OwnerTransferredLeavePreviousRoundTrip) {
  auto encoded = GroupMembershipCodec::EncodeOwnerTransferred("group:g", "relay:bob", 4, true);
  ASSERT_TRUE(encoded);
  auto decoded = GroupMembershipCodec::DecodeOwnerTransferred(*encoded);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->group_id, "group:g");
  EXPECT_EQ(decoded->new_owner_identity, "relay:bob");
  EXPECT_EQ(decoded->roster_epoch, 4u);
  EXPECT_TRUE(decoded->leave_previous);

  ThreadMessage message;
  message.content_type = ChatContentType::System;
  message.payload_json = ControlPayload("owner_transferred", *encoded);
  auto from_message = GroupMembershipCodec::DecodeOwnerTransferredFromMessage(message);
  ASSERT_TRUE(from_message);
  EXPECT_TRUE(from_message->leave_previous);
}

TEST(GroupMembershipCodecTest, DecodeMemberLeftAndRemoved) {
  auto left_detail = GroupMembershipCodec::EncodeMemberLeft("group:g", "relay:bob", 3);
  ASSERT_TRUE(left_detail);
  ThreadMessage left_msg;
  left_msg.content_type = ChatContentType::System;
  left_msg.payload_json = ControlPayload("member_left", *left_detail);
  auto left = GroupMembershipCodec::DecodeMemberLeftFromMessage(left_msg);
  ASSERT_TRUE(left);
  EXPECT_EQ(left->member_identity, "relay:bob");
  EXPECT_EQ(left->roster_epoch, 3u);

  auto removed_detail = GroupMembershipCodec::EncodeMemberRemoved("group:g", "relay:carol", 5);
  ASSERT_TRUE(removed_detail);
  ThreadMessage removed_msg;
  removed_msg.content_type = ChatContentType::System;
  removed_msg.payload_json = ControlPayload("member_removed", *removed_detail);
  auto removed = GroupMembershipCodec::DecodeMemberRemovedFromMessage(removed_msg);
  ASSERT_TRUE(removed);
  EXPECT_EQ(removed->member_identity, "relay:carol");
}

TEST(GroupMembershipCodecTest, OwnerUnreachableActionsAndResolution) {
  const auto actions = GroupMembershipCodec::BuildOwnerUnreachableChatActions("group:g", "relay:alice");
  ASSERT_EQ(actions.size(), 3u);
  EXPECT_NE(actions[0].payload->find("fork_group"), std::string::npos);
  EXPECT_NE(actions[1].payload->find("message_group_owner"), std::string::npos);
  EXPECT_NE(actions[2].payload->find("dismiss_owner_advisory"), std::string::npos);

  ThreadMessage message;
  message.content_type = ChatContentType::System;
  Object detail;
  detail.set("group_id", "group:g");
  message.payload_json = ControlPayload("group_owner_unreachable", DumpJson(detail));
  message.chat_actions = actions;
  EXPECT_TRUE(GroupMembershipCodec::IsOwnerUnreachableAdvisory(message));
  EXPECT_FALSE(GroupMembershipCodec::IsOwnerUnreachableResolved(message));
  GroupMembershipCodec::ApplyOwnerUnreachableResolution(message);
  EXPECT_TRUE(message.chat_actions.empty());
  EXPECT_TRUE(GroupMembershipCodec::IsOwnerUnreachableResolved(message));
}

TEST(GroupMembershipCodecTest, ApplyInviteResolutionClearsActions) {
  GroupInvitePayload invite;
  invite.invite_nonce = "nonce-r";
  invite.inviter_identity = "relay:alice";
  invite.group_title = "Hike";
  auto detail = GroupMembershipCodec::EncodeInvite(invite);
  ASSERT_TRUE(detail);
  ThreadMessage message;
  message.content_type = ChatContentType::System;
  message.text = "Group invitation: Hike";
  message.payload_json = ControlPayload("group_invite", *detail);
  message.chat_actions = GroupMembershipCodec::BuildInviteChatActions(invite);
  ASSERT_EQ(message.chat_actions.size(), 3u);

  GroupMembershipCodec::ApplyInviteResolution(message, InviteStatus::Accepted, "You joined Hike");
  EXPECT_TRUE(message.chat_actions.empty());
  EXPECT_EQ(message.text, "You joined Hike");
  auto resolution = GroupMembershipCodec::InviteResolutionFromMessage(message);
  ASSERT_TRUE(resolution);
  EXPECT_EQ(*resolution, InviteStatus::Accepted);
}

TEST(GroupPermissionsTest, OwnerCanInviteMemberCannot) {
  EXPECT_TRUE(RoleHasPermission(MemberRole::Owner, GroupPermission::kPermInvite));
  EXPECT_FALSE(RoleHasPermission(MemberRole::Member, GroupPermission::kPermInvite));
  EXPECT_TRUE(RoleHasPermission(MemberRole::Member, GroupPermission::kPermFork));
}

TEST(GroupE2ePayloadCodecTest, DetectsGroupEnvelope) {
  RelayEnvelope envelope;
  envelope.route.kind = "group";
  envelope.route.group_id = "group:test";
  EXPECT_TRUE(GroupE2ePayloadCodec::IsGroupEnvelope(envelope));
  envelope.route.kind = "direct";
  envelope.route.group_id = std::nullopt;
  EXPECT_FALSE(GroupE2ePayloadCodec::IsGroupEnvelope(envelope));
}

} // namespace
} // namespace pbr
