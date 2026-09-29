#include "domain/mesh/l4/call_media/CallMediaFrameCrypto.h"
#include "domain/mesh/l4/media_relay/client/MediaRelayFrameCrypto.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

const ByteVector kKey(32, 0x42);
const std::vector<uint8_t> kOpus = {1, 2, 3, 4, 5};

TEST(MediaRelayFrameCryptoTest, SealOpenRoundTripBindsContextStreamAndChannel) {
  const std::string ctx = "broadcast-media|show-1|live:show-1";
  auto body = SealMediaRelayFrame(kKey, ctx, 1, 77, 9, 1, 0, kOpus);
  ASSERT_TRUE(body);
  auto opened = OpenMediaRelayFrame(kKey, ctx, 1, 77, 0, *body);
  ASSERT_TRUE(opened) << opened.error().message;
  EXPECT_EQ(opened->payload, kOpus);
  EXPECT_EQ(opened->seq, 9u);
  EXPECT_EQ(opened->mark, 1);
  EXPECT_FALSE(OpenMediaRelayFrame(kKey, "broadcast-media|show-1|live:other", 1, 77, 0, *body)) << "session";
  EXPECT_FALSE(OpenMediaRelayFrame(kKey, ctx, 2, 77, 0, *body)) << "epoch";
  EXPECT_FALSE(OpenMediaRelayFrame(kKey, ctx, 1, 78, 0, *body)) << "stream";
  EXPECT_FALSE(OpenMediaRelayFrame(kKey, ctx, 1, 77, 1, *body)) << "channel";
}

// L006: a broadcast frame never opens as a call frame (and the reverse), even with the same key.
TEST(MediaRelayFrameCryptoTest, FeatureLabelsDoNotCrossOpen) {
  auto broadcast = SealMediaRelayFrame(kKey, "broadcast-media|p|c1", 1, 5, 1, 0, 0, kOpus);
  ASSERT_TRUE(broadcast);
  EXPECT_FALSE(DecryptCallMediaSfuFrame(kKey, "c1", 1, 5, 0, *broadcast));
  auto call = EncryptCallMediaSfuFrame(kKey, "c1", 1, 5, 1, 0, 0, kOpus);
  ASSERT_TRUE(call);
  EXPECT_FALSE(OpenMediaRelayFrame(kKey, "broadcast-media|p|c1", 1, 5, 0, *call));
}

// Calls kept their bytes: the call SFU framing is the neutral framing under `call-media-sfu|<id>`.
TEST(MediaRelayFrameCryptoTest, CallSfuFramingIsTheNeutralFramingWithTheCallLabel) {
  auto neutral = SealMediaRelayFrame(kKey, "call-media-sfu|call:1", 3, 11, 42, 0, 1, kOpus);
  ASSERT_TRUE(neutral);
  auto as_call = DecryptCallMediaSfuFrame(kKey, "call:1", 3, 11, 1, *neutral);
  ASSERT_TRUE(as_call) << as_call.error().message;
  EXPECT_EQ(as_call->payload, kOpus);
  auto call = EncryptCallMediaSfuFrame(kKey, "call:1", 3, 11, 43, 0, 1, kOpus);
  ASSERT_TRUE(call);
  EXPECT_TRUE(OpenMediaRelayFrame(kKey, "call-media-sfu|call:1", 3, 11, 1, *call));
}

TEST(MediaRelayFrameCryptoTest, RejectsTruncatedAndLegacyBodies) {
  EXPECT_FALSE(OpenMediaRelayFrame(kKey, "x", 1, 1, 0, std::vector<uint8_t>{2, 0, 0}));
  auto v1 = EncryptCallMediaAudioFrameV1(kKey, "c", 1, 1, 0, kOpus);
  ASSERT_TRUE(v1);
  EXPECT_FALSE(OpenMediaRelayFrame(kKey, "call-media-sfu|c", 1, 1, 0, *v1)) << "v2 only";
  EXPECT_FALSE(SealMediaRelayFrame(ByteVector{}, "x", 1, 1, 1, 0, 0, kOpus)) << "key required";
}

} // namespace
} // namespace pbr
