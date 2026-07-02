#include "base/crypto/CryptoUtil.h"
#include "base/messaging/EnvelopeSigner.h"
#include "base/messaging/MessagingJson.h"
#include "base/people/Ed25519Signer.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

RelayEnvelope MakeE2eVectorEnvelope() {
  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = "660e8400-e29b-41d4-a716-446655440001";
  envelope.sender_relay_id = "relay:alice123";
  envelope.sender_contact_id = "relay:alice123";
  envelope.route.kind = "direct";
  envelope.route.channel = ThreadChannel::E2e;
  envelope.body.e2e.payload_b64 =
      "AQABAgMEBQYHCAkKCwwNDg8QERITFBUWF1vPScnCPAVe+dnJiV9kKBztMM3qj/Hi+RhfLy6wlhU=";
  envelope.sender_seq = 42;
  envelope.session_epoch = 1;
  envelope.timestamp = 1719662400456;
  return envelope;
}

} // namespace

TEST(EnvelopeSignerTest, BodyHashMatchesFrozenVector) {
  const RelayEnvelope envelope = MakeE2eVectorEnvelope();
  auto body_hash = EnvelopeSigner::BodyHash(envelope.body);
  ASSERT_TRUE(body_hash);
  EXPECT_EQ(BytesToHex(*body_hash), "b09daad4a14b17961c834c3b027c3d03ef49a0b1f3bffaa7c8c22da097a8042e");
}

TEST(EnvelopeSignerTest, BuildSignBytesUsesE2eChannelZero) {
  const RelayEnvelope envelope = MakeE2eVectorEnvelope();
  auto sign_bytes = EnvelopeSigner::BuildSignBytes(envelope);
  ASSERT_TRUE(sign_bytes);
  ASSERT_GE(sign_bytes->size(), 38u);
  EXPECT_EQ((*sign_bytes)[36], 0u);
  EXPECT_EQ((*sign_bytes)[37], 0u);
}

TEST(EnvelopeSignerTest, FrozenPayloadBodyHashAndSignRoundTrip) {
  RelayEnvelope envelope = MakeE2eVectorEnvelope();
  auto body_hash = EnvelopeSigner::BodyHash(envelope.body);
  ASSERT_TRUE(body_hash);
  EXPECT_EQ(BytesToHex(*body_hash), "b09daad4a14b17961c834c3b027c3d03ef49a0b1f3bffaa7c8c22da097a8042e");

  auto private_key = HexToBytes(
      "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  ASSERT_TRUE(private_key);
  auto sign_bytes = EnvelopeSigner::BuildSignBytes(envelope);
  ASSERT_TRUE(sign_bytes);
  auto signature =
      Ed25519Signer::Sign(std::string(sign_bytes->begin(), sign_bytes->end()), *private_key);
  ASSERT_TRUE(signature);
  envelope.signature = *signature;

  auto verified = EnvelopeSigner::Verify(envelope, "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=");
  ASSERT_TRUE(verified);
  EXPECT_TRUE(*verified);
}

TEST(EnvelopeSignerTest, SignAndVerifyRoundTrip) {
  RelayEnvelope envelope = MakeE2eVectorEnvelope();
  envelope.message_id = "770e8400-e29b-41d4-a716-446655440002";

  const auto private_key = HexToBytes(
      "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  ASSERT_TRUE(private_key);
  auto sign_bytes = EnvelopeSigner::BuildSignBytes(envelope);
  ASSERT_TRUE(sign_bytes);
  const auto signature =
      Ed25519Signer::Sign(std::string(sign_bytes->begin(), sign_bytes->end()), *private_key);
  ASSERT_TRUE(signature);
  envelope.signature = *signature;

  const auto verified = EnvelopeSigner::Verify(envelope, "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=");
  ASSERT_TRUE(verified);
  EXPECT_TRUE(*verified);
}

} // namespace pbr
