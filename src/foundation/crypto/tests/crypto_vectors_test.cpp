#include "foundation/crypto/CanonicalAad.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/EncryptedPayload.h"
#include "foundation/crypto/MessageCipher.h"
#include "foundation/crypto/PskFingerprint.h"
#include "foundation/crypto/ReplayWindow.h"
#include "foundation/crypto/SessionKeyDeriver.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

ByteVector FixedMasterPsk() {
  const auto bytes = HexToBytes("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  EXPECT_TRUE(bytes);
  return bytes.value();
}

ByteVector FixedSessionKey() {
  const auto bytes = HexToBytes("f7dab69eb0c862df230bc383c1dea363637a6caf2d46d7b57d1b45b5526a7358");
  EXPECT_TRUE(bytes);
  return bytes.value();
}

ByteVector FixedNonce() {
  const auto bytes = HexToBytes("000102030405060708090a0b0c0d0e0f1011121314151617");
  EXPECT_TRUE(bytes);
  return bytes.value();
}

ByteVector VectorAPlaintext() {
  const auto bytes = HexToBytes("0100000000000000000548656c6c6f");
  EXPECT_TRUE(bytes);
  return bytes.value();
}

TEST(CryptoVectors, HkdfSessionKeyMatchesFixture) {
  const auto session_key = SessionKeyDeriver::Derive(FixedMasterPsk(), CryptoChannel::E2e, 1);
  ASSERT_TRUE(session_key);
  EXPECT_EQ(BytesToHex(session_key.value()), "f7dab69eb0c862df230bc383c1dea363637a6caf2d46d7b57d1b45b5526a7358");
  EXPECT_EQ(SessionKeyDeriver::BuildHkdfInfo(CryptoChannel::E2e, 1), "channel:e2e|epoch:1");
}

TEST(CryptoVectors, AeadRoundTripWithFixedInputs) {
  AadFields fields;
  fields.channel = CryptoChannel::E2e;
  fields.peer_contact_id = "relay:bob456";
  fields.message_id = "660e8400-e29b-41d4-a716-446655440001";
  fields.sender_contact_id = "relay:alice123";
  fields.sender_seq = 42;
  fields.session_epoch = 1;
  fields.timestamp = 1719662400456;

  const auto aad = CanonicalAad::Build(fields);
  ASSERT_TRUE(aad);

  const auto encrypted =
      MessageCipher::Encrypt(FixedSessionKey(), VectorAPlaintext(), aad.value(), FixedNonce());
  ASSERT_TRUE(encrypted);

  const auto blob = EncryptedPayload::EncodeBlob(encrypted.value());
  ASSERT_TRUE(blob);
  EXPECT_EQ(blob.value().size(), 1 + kAeadNonceSize + encrypted.value().ciphertext.size());

  const auto decoded_blob = EncryptedPayload::DecodeBlob(blob.value());
  ASSERT_TRUE(decoded_blob);
  const auto plaintext = MessageCipher::Decrypt(FixedSessionKey(), decoded_blob.value(), aad.value());
  ASSERT_TRUE(plaintext);
  EXPECT_EQ(plaintext.value(), VectorAPlaintext());

  const auto payload_b64 = EncryptedPayload::EncodeBase64(blob.value());
  const auto decoded_from_b64 = EncryptedPayload::DecodeBase64(payload_b64);
  ASSERT_TRUE(decoded_from_b64);
  EXPECT_EQ(decoded_from_b64.value(), blob.value());
}

TEST(CryptoVectors, AeadTamperFails) {
  AadFields fields;
  fields.channel = CryptoChannel::E2e;
  fields.peer_contact_id = "relay:bob456";
  fields.message_id = "660e8400-e29b-41d4-a716-446655440001";
  fields.sender_contact_id = "relay:alice123";
  fields.sender_seq = 42;
  fields.session_epoch = 1;
  fields.timestamp = 1719662400456;

  const auto aad = CanonicalAad::Build(fields);
  ASSERT_TRUE(aad);
  const auto encrypted =
      MessageCipher::Encrypt(FixedSessionKey(), VectorAPlaintext(), aad.value(), FixedNonce());
  ASSERT_TRUE(encrypted);

  EncryptedBlob tampered = encrypted.value();
  tampered.ciphertext[0] ^= 0x01;
  EXPECT_FALSE(MessageCipher::Decrypt(FixedSessionKey(), tampered, aad.value()));
}

TEST(CryptoVectors, WrongAadFailsDecrypt) {
  AadFields fields;
  fields.channel = CryptoChannel::E2e;
  fields.peer_contact_id = "relay:bob456";
  fields.message_id = "660e8400-e29b-41d4-a716-446655440001";
  fields.sender_contact_id = "relay:alice123";
  fields.sender_seq = 42;
  fields.session_epoch = 1;
  fields.timestamp = 1719662400456;

  const auto aad = CanonicalAad::Build(fields);
  ASSERT_TRUE(aad);
  const auto encrypted =
      MessageCipher::Encrypt(FixedSessionKey(), VectorAPlaintext(), aad.value(), FixedNonce());
  ASSERT_TRUE(encrypted);

  fields.sender_seq = 43;
  const auto wrong_aad = CanonicalAad::Build(fields);
  ASSERT_TRUE(wrong_aad);
  EXPECT_FALSE(MessageCipher::Decrypt(FixedSessionKey(), encrypted.value(), wrong_aad.value()));
}

TEST(CryptoVectors, CanonicalAadRoundTrip) {
  AadFields fields;
  fields.channel = CryptoChannel::E2e;
  fields.peer_contact_id = "relay:bob456";
  fields.message_id = "660e8400-e29b-41d4-a716-446655440001";
  fields.sender_contact_id = "relay:alice123";
  fields.sender_seq = 42;
  fields.session_epoch = 1;
  fields.timestamp = 1719662400456;

  const auto built = CanonicalAad::Build(fields);
  ASSERT_TRUE(built);
  const auto parsed = CanonicalAad::Parse(built.value());
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value().channel, fields.channel);
  EXPECT_EQ(parsed.value().peer_contact_id, fields.peer_contact_id);
  EXPECT_EQ(parsed.value().message_id, fields.message_id);
  EXPECT_EQ(parsed.value().sender_contact_id, fields.sender_contact_id);
  EXPECT_EQ(parsed.value().sender_seq, fields.sender_seq);
  EXPECT_EQ(parsed.value().session_epoch, fields.session_epoch);
  EXPECT_EQ(parsed.value().timestamp, fields.timestamp);
}

TEST(CryptoVectors, ReplayWindowAcceptsOutOfOrderWithinWindow) {
  ReplayWindow window(32);
  EXPECT_TRUE(window.Accept(1));
  EXPECT_TRUE(window.Accept(3));
  EXPECT_FALSE(window.Accept(3));
  EXPECT_TRUE(window.Accept(2));
  EXPECT_EQ(window.LastContiguous(), 3u);
}

TEST(CryptoVectors, PskFingerprintDeterministic) {
  const auto digest = PskFingerprint::Compute(FixedMasterPsk());
  ASSERT_TRUE(digest);
  const std::string display = PskFingerprint::FormatDisplay(digest.value());
  const auto parsed = PskFingerprint::ParseDisplay(display);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value(), digest.value());
}

} // namespace
} // namespace pbr
