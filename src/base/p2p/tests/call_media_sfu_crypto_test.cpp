#include "base/p2p/CallMediaFrameCrypto.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

ByteVector FakeKey() {
  return ByteVector(32, 0x42);
}

TEST(CallMediaSfuCryptoTest, RoundTrip) {
  const std::string call_id = "call:sfu-1";
  const uint32_t epoch = 2;
  const uint32_t stream = 0xabcdu;
  const std::vector<uint8_t> opus = {1, 2, 3, 4, 5, 9};
  auto sealed = EncryptCallMediaSfuAudioFrame(FakeKey(), call_id, epoch, stream, /*seq=*/7, /*mark=*/0, opus);
  ASSERT_TRUE(sealed) << sealed.error().message;
  auto plain = DecryptCallMediaSfuAudioFrame(FakeKey(), call_id, epoch, stream, *sealed);
  ASSERT_TRUE(plain) << plain.error().message;
  EXPECT_EQ(*plain, opus);
}

TEST(CallMediaSfuCryptoTest, StreamIdBoundInAad) {
  const std::string call_id = "call:sfu-2";
  const std::vector<uint8_t> opus = {9, 8, 7};
  auto sealed = EncryptCallMediaSfuAudioFrame(FakeKey(), call_id, 1, /*stream=*/10, 1, 0, opus);
  ASSERT_TRUE(sealed);
  auto wrong_stream = DecryptCallMediaSfuAudioFrame(FakeKey(), call_id, 1, /*stream=*/11, *sealed);
  EXPECT_FALSE(wrong_stream);
}

TEST(CallMediaSfuCryptoTest, DistinctFromDirectAad) {
  EXPECT_NE(BuildCallMediaFrameAad("c", 1, 2), BuildCallMediaSfuFrameAad("c", 1, 0, 2));
}

} // namespace
} // namespace pbr
