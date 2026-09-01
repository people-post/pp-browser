#include "lib/amp/L2/SessionControl.h"

#include <gtest/gtest.h>

namespace pp::amp {
namespace {

TEST(SessionControlCodecTest, RoundTripRequestAndAck) {
  SessionRekeyMessage request;
  request.kind = SessionControlKind::RekeyRequest;
  request.target_epoch = 2;
  auto encoded = SessionControlCodec::Encode(request);
  ASSERT_TRUE(static_cast<bool>(encoded));
  EXPECT_TRUE(SessionControlCodec::LooksLike(*encoded));
  auto decoded = SessionControlCodec::Decode(*encoded);
  ASSERT_TRUE(static_cast<bool>(decoded));
  EXPECT_EQ(decoded->kind, SessionControlKind::RekeyRequest);
  EXPECT_EQ(decoded->target_epoch, 2u);

  SessionRekeyMessage ack;
  ack.kind = SessionControlKind::RekeyAck;
  ack.target_epoch = 2;
  auto ack_wire = SessionControlCodec::Encode(ack);
  ASSERT_TRUE(static_cast<bool>(ack_wire));
  auto ack_decoded = SessionControlCodec::Decode(*ack_wire);
  ASSERT_TRUE(static_cast<bool>(ack_decoded));
  EXPECT_EQ(ack_decoded->kind, SessionControlKind::RekeyAck);
}

TEST(SessionControlCodecTest, DistinguishesFromCapabilityVersionOne) {
  const std::vector<uint8_t> cap_like = {1, 0, 0, 0, 0};
  EXPECT_FALSE(SessionControlCodec::LooksLike(cap_like));
}

} // namespace
} // namespace pp::amp
