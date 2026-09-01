#include "base/mesh/CallMediaFrameCrypto.h"

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

TEST(CallMediaSfuCryptoTest, DirectV2AudioAndVideoRoundTrip) {
  const std::string call_id = "call:direct-v2";
  const std::vector<uint8_t> opus = {0x11, 0x22};
  auto audio = EncryptCallMediaFrame(FakeKey(), call_id, 1, 3, 0, kCallMediaChannelAudio, opus);
  ASSERT_TRUE(audio) << audio.error().message;
  auto decoded_audio = DecryptCallMediaFrame(FakeKey(), call_id, 1, *audio);
  ASSERT_TRUE(decoded_audio) << decoded_audio.error().message;
  EXPECT_EQ(decoded_audio->channel, kCallMediaChannelAudio);
  EXPECT_EQ(decoded_audio->payload, opus);

  std::vector<uint8_t> au(20 * 1024, 0x5a);
  auto video = EncryptCallMediaFrame(FakeKey(), call_id, 1, 4, 1, kCallMediaChannelVideoLo, au);
  ASSERT_TRUE(video) << video.error().message;
  EXPECT_GT(video->size(), 16 * 1024u);
  auto decoded_video = DecryptCallMediaFrame(FakeKey(), call_id, 1, *video);
  ASSERT_TRUE(decoded_video) << decoded_video.error().message;
  EXPECT_EQ(decoded_video->channel, kCallMediaChannelVideoLo);
  EXPECT_EQ(decoded_video->mark, 1);
  EXPECT_EQ(decoded_video->payload, au);

  auto as_audio = DecryptCallMediaAudioFrame(FakeKey(), call_id, 1, *video);
  EXPECT_FALSE(as_audio);
}

TEST(CallMediaSfuCryptoTest, OneSealDecryptsForEveryParticipantWithSharedKey) {
  const std::string call_id = "call:group-one-key";
  std::vector<uint8_t> au(4096, 0x7e);
  auto sealed = EncryptCallMediaSfuFrame(FakeKey(), call_id, 1, 10, 1, 1, kCallMediaChannelVideoLo, au);
  ASSERT_TRUE(sealed);
  // Two subscribers, same epoch key — not per-target ciphertext.
  auto peer_a = DecryptCallMediaSfuFrame(FakeKey(), call_id, 1, 10, kCallMediaChannelVideoLo, *sealed);
  auto peer_b = DecryptCallMediaSfuFrame(FakeKey(), call_id, 1, 10, kCallMediaChannelVideoLo, *sealed);
  ASSERT_TRUE(peer_a);
  ASSERT_TRUE(peer_b);
  EXPECT_EQ(peer_a->payload, au);
  EXPECT_EQ(peer_b->payload, au);
  ByteVector other_key(32, 0x24);
  EXPECT_FALSE(DecryptCallMediaSfuFrame(other_key, call_id, 1, 10, kCallMediaChannelVideoLo, *sealed));
}

TEST(CallMediaSfuCryptoTest, ChannelBoundInAad) {
  const std::string call_id = "call:ch-aad";
  const std::vector<uint8_t> payload = {1, 2, 3};
  auto sealed = EncryptCallMediaSfuFrame(FakeKey(), call_id, 1, 10, 1, 0, kCallMediaChannelVideoLo, payload);
  ASSERT_TRUE(sealed);
  auto wrong_ch = DecryptCallMediaSfuFrame(FakeKey(), call_id, 1, 10, kCallMediaChannelAudio, *sealed);
  EXPECT_FALSE(wrong_ch);
  auto ok = DecryptCallMediaSfuFrame(FakeKey(), call_id, 1, 10, kCallMediaChannelVideoLo, *sealed);
  ASSERT_TRUE(ok) << ok.error().message;
  EXPECT_EQ(ok->payload, payload);
}

TEST(CallMediaSfuCryptoTest, DecryptV1AudioStillWorks) {
  EXPECT_NE(BuildCallMediaFrameAad("c", 1, 2), BuildCallMediaFrameAad("c", 1, 2, 0));
  auto v1 = EncryptCallMediaAudioFrameV1(FakeKey(), "c", 1, 2, 0, {9, 9});
  ASSERT_TRUE(v1) << v1.error().message;
  EXPECT_EQ((*v1)[0], kCallMediaFrameVersionV1);
  auto decoded = DecryptCallMediaFrame(FakeKey(), "c", 1, *v1);
  ASSERT_TRUE(decoded) << decoded.error().message;
  EXPECT_EQ(decoded->channel, kCallMediaChannelAudio);
  EXPECT_EQ(decoded->payload, (std::vector<uint8_t>{9, 9}));
  auto plain = DecryptCallMediaAudioFrame(FakeKey(), "c", 1, *v1);
  ASSERT_TRUE(plain);
  EXPECT_EQ(*plain, (std::vector<uint8_t>{9, 9}));
}

} // namespace
} // namespace pbr
