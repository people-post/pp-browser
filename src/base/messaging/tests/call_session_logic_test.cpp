#include "base/messaging/CallControlCodec.h"
#include "base/messaging/CallSessionLogic.h"
#include "base/messaging/CallTypes.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(CallSessionLogicTest, SelectEpochCoordinatorMinIdentity) {
  EXPECT_FALSE(CallSessionLogic::SelectEpochCoordinator({}).has_value());
  EXPECT_EQ(*CallSessionLogic::SelectEpochCoordinator({"relay:bob"}), "relay:bob");
  EXPECT_EQ(*CallSessionLogic::SelectEpochCoordinator({"relay:bob", "relay:alice", "relay:carol"}),
            "relay:alice");
  EXPECT_EQ(*CallSessionLogic::SelectEpochCoordinator({"relay:z", "", "relay:a"}), "relay:a");
}

TEST(CallSessionLogicTest, TransitionOnRemoteJoined) {
  EXPECT_EQ(CallSessionLogic::TransitionOnRemoteJoined(CallSessionState::Ringing), CallSessionState::Active);
  EXPECT_EQ(CallSessionLogic::TransitionOnRemoteJoined(CallSessionState::Active), CallSessionState::Active);
  EXPECT_EQ(CallSessionLogic::TransitionOnRemoteJoined(CallSessionState::Ended), CallSessionState::Ended);
}

TEST(CallSessionLogicTest, TransitionOnLeaveHostless) {
  EXPECT_EQ(CallSessionLogic::TransitionOnLeave(CallSessionState::Active, 0), CallSessionState::Ended);
  EXPECT_EQ(CallSessionLogic::TransitionOnLeave(CallSessionState::Active, 1), CallSessionState::Ended);
  EXPECT_EQ(CallSessionLogic::TransitionOnLeave(CallSessionState::Active, 2), CallSessionState::Active);
  EXPECT_EQ(CallSessionLogic::TransitionOnLeave(CallSessionState::Ringing, 1), CallSessionState::Ringing);
  EXPECT_EQ(CallSessionLogic::TransitionOnLeave(CallSessionState::Ringing, 0), CallSessionState::Ended);
  EXPECT_EQ(CallSessionLogic::TransitionOnLeave(CallSessionState::Ended, 0), CallSessionState::Ended);
}

TEST(CallSessionLogicTest, InviteExpiry) {
  PendingCallInvite invite;
  invite.status = "pending";
  invite.expires_at = 1000;
  EXPECT_FALSE(CallSessionLogic::IsInviteExpired(invite, 999));
  EXPECT_TRUE(CallSessionLogic::IsInviteExpired(invite, 1000));
  EXPECT_TRUE(CallSessionLogic::IsInviteExpired(invite, 1001));

  PendingCallInvite open;
  open.status = "pending";
  EXPECT_FALSE(CallSessionLogic::IsInviteExpired(open, 999999));

  auto expired = CallSessionLogic::ExpirePendingInvites({invite, open}, 1000);
  ASSERT_EQ(expired.size(), 2u);
  EXPECT_EQ(expired[0].status, "expired");
  EXPECT_EQ(expired[1].status, "pending");
}

TEST(CallSessionLogicTest, RelayAgeAndStaleDrop) {
  EXPECT_EQ(CallSessionLogic::RelayInviteAgeMs(1000, 1060'000), 1059'000);
  EXPECT_EQ(CallSessionLogic::DeltaRelayReceiverMs(2000, 1500), 500);

  CallInviteDetail invite;
  invite.expires_at = 1000;
  // Relay age beyond TTL + slack → drop.
  EXPECT_TRUE(CallSessionLogic::ShouldDropStaleInvite(invite, 999999, 1000, 1000 + kDefaultCallInviteTtlMs +
                                                                             kCallInviteRelayAgeSlackMs + 1));
  EXPECT_FALSE(CallSessionLogic::ShouldDropStaleInvite(invite, 999999, 1000,
                                                       1000 + kDefaultCallInviteTtlMs));

  // Direct path: within skew slack of wire expiry → keep; far past → drop.
  EXPECT_FALSE(CallSessionLogic::ShouldDropStaleInvite(invite, 1000 + kCallInviteWireSkewSlackMs, std::nullopt,
                                                       std::nullopt));
  EXPECT_TRUE(CallSessionLogic::ShouldDropStaleInvite(
      invite, 1000 + kCallInviteWireSkewSlackMs + 1, std::nullopt, std::nullopt));
}

TEST(CallSessionLogicTest, CanAcceptJoinRespectsCap) {
  EXPECT_TRUE(CallSessionLogic::CanAcceptJoin(0));
  EXPECT_TRUE(CallSessionLogic::CanAcceptJoin(kCallEngineeringMaxJoined - 1));
  EXPECT_FALSE(CallSessionLogic::CanAcceptJoin(kCallEngineeringMaxJoined));
  EXPECT_TRUE(CallSessionLogic::CanAcceptJoin(15, 16));
  EXPECT_FALSE(CallSessionLogic::CanAcceptJoin(16, 16));
}

TEST(CallControlTypeTest, WireRoundTrip) {
  EXPECT_EQ(CallControlTypeToWire(CallControlType::CallInvite), "call_invite");
  EXPECT_EQ(CallControlTypeFromWire("call_join"), CallControlType::CallAccept);
  EXPECT_EQ(CallControlTypeFromWire("call_started"), CallControlType::CallStarted);
  EXPECT_FALSE(CallControlTypeFromWire("member_joined").has_value());
}

TEST(CallControlTypeTest, WireRoundTripSdpAndIce) {
  EXPECT_EQ(CallControlTypeToWire(CallControlType::CallSdp), "call_sdp");
  EXPECT_EQ(CallControlTypeToWire(CallControlType::CallIce), "call_ice");
  EXPECT_EQ(CallControlTypeFromWire("call_sdp"), CallControlType::CallSdp);
  EXPECT_EQ(CallControlTypeFromWire("call_ice"), CallControlType::CallIce);
  EXPECT_EQ(CallControlTypeToWire(CallControlType::CallSfuAttach), "call_sfu_attach");
  EXPECT_EQ(CallControlTypeFromWire("call_sfu_attach"), CallControlType::CallSfuAttach);
  EXPECT_EQ(CallControlTypeToWire(CallControlType::CallSfuAttachFailed), "call_sfu_attach_failed");
  EXPECT_EQ(CallControlTypeFromWire("call_sfu_attach_failed"), CallControlType::CallSfuAttachFailed);
  EXPECT_EQ(CallControlTypeToWire(CallControlType::CallHopRefuse), "call_hop_refuse");
  EXPECT_EQ(CallControlTypeFromWire("call_hop_refuse"), CallControlType::CallHopRefuse);
}

TEST(CallControlCodecTest, SdpDetailRoundTrip) {
  CallSdpDetail detail;
  detail.call_id = "call:abc";
  detail.identity = "relay:alice";
  detail.sdp_type = "offer";
  detail.sdp = "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\n";

  auto encoded = CallControlCodec::EncodeSdp(detail);
  ASSERT_TRUE(encoded);
  auto decoded = CallControlCodec::DecodeSdp(*encoded);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->call_id, detail.call_id);
  EXPECT_EQ(decoded->identity, detail.identity);
  EXPECT_EQ(decoded->sdp_type, detail.sdp_type);
  EXPECT_EQ(decoded->sdp, detail.sdp);

  EXPECT_FALSE(CallControlCodec::DecodeSdp(R"({"call_id":"call:abc"})"));
}

TEST(CallControlCodecTest, InviteAcceptListenMultiaddrsRoundTrip) {
  CallInviteDetail invite;
  invite.call_id = "call:abc";
  invite.inviter_identity = "relay:alice";
  invite.invitee_identity = "relay:bob";
  invite.listen_multiaddrs = {"/ip4/192.168.1.10/tcp/18517/p2p/12D3KooWAlice"};
  invite.libp2p_peer_id = "12D3KooWAlice";
  invite.caps.present = true;
  invite.caps.media_relay = true;

  auto encoded_invite = CallControlCodec::EncodeInvite(invite);
  ASSERT_TRUE(encoded_invite);
  EXPECT_NE(encoded_invite->find("\"caps\""), std::string::npos);
  EXPECT_NE(encoded_invite->find("libp2p_peer_id"), std::string::npos);
  auto decoded_invite = CallControlCodec::DecodeInvite(*encoded_invite);
  ASSERT_TRUE(decoded_invite);
  ASSERT_EQ(decoded_invite->listen_multiaddrs.size(), 1u);
  EXPECT_EQ(decoded_invite->listen_multiaddrs[0], invite.listen_multiaddrs[0]);
  EXPECT_EQ(decoded_invite->libp2p_peer_id, "12D3KooWAlice");
  EXPECT_TRUE(decoded_invite->caps.present);
  EXPECT_TRUE(decoded_invite->caps.media_relay);

  CallAcceptDetail accept;
  accept.call_id = "call:abc";
  accept.identity = "relay:bob";
  accept.listen_multiaddrs = {"/ip4/192.168.1.20/tcp/18517/p2p/12D3KooWBob"};
  accept.libp2p_peer_id = "12D3KooWBob";
  accept.caps.present = true;
  accept.caps.media_relay = false;
  auto encoded_accept = CallControlCodec::EncodeAccept(accept);
  ASSERT_TRUE(encoded_accept);
  auto decoded_accept = CallControlCodec::DecodeAccept(*encoded_accept);
  ASSERT_TRUE(decoded_accept);
  ASSERT_EQ(decoded_accept->listen_multiaddrs.size(), 1u);
  EXPECT_EQ(decoded_accept->listen_multiaddrs[0], accept.listen_multiaddrs[0]);
  EXPECT_EQ(decoded_accept->libp2p_peer_id, "12D3KooWBob");
  EXPECT_TRUE(decoded_accept->caps.present);
  EXPECT_FALSE(decoded_accept->caps.media_relay);

  auto old_invite = CallControlCodec::DecodeInvite(
      R"({"call_id":"call:x","inviter_identity":"a","invitee_identity":"b","media_mode":"voice"})");
  ASSERT_TRUE(old_invite);
  EXPECT_FALSE(old_invite->caps.present);
  EXPECT_FALSE(old_invite->caps.media_relay);
}

} // namespace
} // namespace pbr
