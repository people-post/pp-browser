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

TEST(CallControlCodecTest, IceDetailRoundTrip) {
  CallIceDetail detail;
  detail.call_id = "call:abc";
  detail.identity = "relay:bob";
  detail.candidate = "candidate:1 1 UDP 2130706431 10.0.0.1 5000 typ host";
  detail.mid = "audio";

  auto encoded = CallControlCodec::EncodeIce(detail);
  ASSERT_TRUE(encoded);
  auto decoded = CallControlCodec::DecodeIce(*encoded);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->call_id, detail.call_id);
  EXPECT_EQ(decoded->identity, detail.identity);
  EXPECT_EQ(decoded->candidate, detail.candidate);
  EXPECT_EQ(decoded->mid, detail.mid);

  EXPECT_FALSE(CallControlCodec::DecodeIce(R"({"call_id":"call:abc"})"));
}

} // namespace
} // namespace pbr
