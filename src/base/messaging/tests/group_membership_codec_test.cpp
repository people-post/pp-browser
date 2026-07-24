#include "base/messaging/GroupMembershipCodec.h"
#include "base/messaging/GroupE2ePayloadCodec.h"
#include "base/messaging/GroupTypes.h"
#include "base/messaging/ThreadTypes.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace pbr {
namespace {

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
  message.payload_json =
      nlohmann::json({{"control_type", "group_invite_accept"}, {"detail", *detail}}).dump();
  auto decoded = GroupMembershipCodec::DecodeInviteResponseFromMessage(message);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->invite_nonce, "nonce-xyz");
  EXPECT_EQ(decoded->group_id, "group:abc");
  EXPECT_EQ(decoded->control_type, GroupMembershipControlType::GroupInviteAccept);

  message.payload_json =
      nlohmann::json({{"control_type", "group_invite_decline"}, {"detail", *detail}}).dump();
  auto declined = GroupMembershipCodec::DecodeInviteResponseFromMessage(message);
  ASSERT_TRUE(declined);
  EXPECT_EQ(declined->control_type, GroupMembershipControlType::GroupInviteDecline);
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
