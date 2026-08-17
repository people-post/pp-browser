#include "base/crypto/CryptoTypes.h"
#include "base/messaging/PskRotateCodec.h"
#include "base/messaging/ThreadTypes.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace pbr {
namespace {

PskRotateDetail ValidDetail() {
  PskRotateDetail detail;
  detail.rotation_id = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
  detail.new_epoch = 2;
  detail.wrap_kind = kPskRotateWrapAccountKem;
  detail.thread_kem_pk_b64 = "dGVzdA==";
  detail.key_init_hash = std::string(64, 'a');
  return detail;
}

TEST(PskRotateCodecTest, EncodeDecodeRoundTrip) {
  const PskRotateDetail detail = ValidDetail();
  auto json = PskRotateCodec::EncodePayloadJson(detail);
  ASSERT_TRUE(static_cast<bool>(json)) << json.error().message;

  ThreadMessage message;
  message.content_type = ChatContentType::System;
  message.payload_json = *json;
  EXPECT_TRUE(PskRotateCodec::IsPskRotateMessage(message));
  auto decoded = PskRotateCodec::Decode(message);
  ASSERT_TRUE(static_cast<bool>(decoded)) << decoded.error().message;
  EXPECT_EQ(decoded->rotation_id, detail.rotation_id);
  EXPECT_EQ(decoded->new_epoch, 2u);
  EXPECT_EQ(decoded->wrap_kind, kPskRotateWrapAccountKem);
  EXPECT_EQ(decoded->thread_kem_pk_b64, detail.thread_kem_pk_b64);
  EXPECT_EQ(decoded->key_init_hash, detail.key_init_hash);
}

TEST(PskRotateCodecTest, RejectsObjectDetail) {
  ThreadMessage message;
  message.content_type = ChatContentType::System;
  message.payload_json = nlohmann::json({{"control_type", kPskRotateControlType},
                                         {"detail", {{"rotation_id", "x"}}}})
                             .dump();
  auto decoded = PskRotateCodec::Decode(message);
  ASSERT_FALSE(static_cast<bool>(decoded));
  EXPECT_NE(decoded.error().message.find("JSON string"), std::string::npos);
}

TEST(PskRotateCodecTest, RotationIdWinnerIsLexicographicallyGreater) {
  EXPECT_TRUE(PskRotateCodec::RotationIdWins("b", "a"));
  EXPECT_FALSE(PskRotateCodec::RotationIdWins("a", "b"));
}

} // namespace
} // namespace pbr
