#include "foundation/crypto/CryptoConstants.h"
#include "domain/messaging/TranscriptBodyCodec.h"
#include "domain/messaging/TranscriptCipher.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize, 0x42);
  return dek;
}

TEST(TranscriptBodyCodecTest, RoundTripPreservesFields) {
  TranscriptBodyPlaintext body;
  body.chat_payload = {0x01, 0x00, 0x05, 'h', 'i'};
  body.text = "visible";
  body.payload_json = R"({"control_type":"system"})";
  body.content_rml = "<p>rml</p>";
  body.chat_actions.push_back(TranscriptChatAction{.label = "Go", .message = "do it"});

  auto encoded = TranscriptBodyCodec::Encode(body);
  ASSERT_TRUE(encoded);
  auto decoded = TranscriptBodyCodec::Decode(*encoded);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->chat_payload, body.chat_payload);
  EXPECT_EQ(decoded->text, body.text);
  EXPECT_EQ(decoded->payload_json, body.payload_json);
  ASSERT_TRUE(decoded->content_rml);
  EXPECT_EQ(*decoded->content_rml, *body.content_rml);
  ASSERT_EQ(decoded->chat_actions.size(), 1u);
  EXPECT_EQ(decoded->chat_actions.front().label, "Go");
}

TEST(TranscriptCipherTest, MessageBodyRoundTripAndAadBind) {
  const ByteVector dek = TestDek();
  const ByteVector plain = {0x01, 0x02, 0x03};
  auto cipher = TranscriptCipher::EncryptMessageBody(dek, "profile-a", "thread-1", "msg-1", plain);
  ASSERT_TRUE(cipher);
  auto round = TranscriptCipher::DecryptMessageBody(dek, "profile-a", "thread-1", "msg-1", *cipher);
  ASSERT_TRUE(round);
  EXPECT_EQ(*round, plain);

  auto wrong_thread =
      TranscriptCipher::DecryptMessageBody(dek, "profile-a", "thread-2", "msg-1", *cipher);
  EXPECT_FALSE(static_cast<bool>(wrong_thread));
}

TEST(TranscriptCipherTest, PreviewRoundTrip) {
  const ByteVector dek = TestDek();
  auto cipher = TranscriptCipher::EncryptPreview(dek, "profile-a", "thread-1", "hello preview");
  ASSERT_TRUE(cipher);
  auto plain = TranscriptCipher::DecryptPreview(dek, "profile-a", "thread-1", *cipher);
  ASSERT_TRUE(plain);
  EXPECT_EQ(*plain, "hello preview");
}

TEST(TranscriptCipherTest, MemoryRoundTrip) {
  const ByteVector dek = TestDek();
  auto cipher = TranscriptCipher::EncryptMemoryValue(dek, "profile-a", "thread-1", "summary", "facts");
  ASSERT_TRUE(cipher);
  auto plain = TranscriptCipher::DecryptMemoryValue(dek, "profile-a", "thread-1", "summary", *cipher);
  ASSERT_TRUE(plain);
  EXPECT_EQ(*plain, "facts");
}

} // namespace
} // namespace pbr
