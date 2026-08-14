#include "base/crypto/CryptoUtil.h"
#include "base/crypto/MlDsa.h"
#include "base/messaging/EnvelopeSigner.h"
#include "base/messaging/MessagingJson.h"

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

TEST(EnvelopeSignerTest, SignAndVerifyRoundTrip) {
  RelayEnvelope envelope = MakeE2eVectorEnvelope();
  envelope.message_id = "770e8400-e29b-41d4-a716-446655440002";

  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys));
  auto sign_bytes = EnvelopeSigner::BuildSignBytes(envelope);
  ASSERT_TRUE(sign_bytes);
  auto signature = MlDsa::Sign(keys->secret_key, *sign_bytes);
  ASSERT_TRUE(signature);
  envelope.signature = Base64Encode(*signature);

  const auto verified = EnvelopeSigner::Verify(envelope, Base64Encode(keys->public_key));
  ASSERT_TRUE(verified);
  EXPECT_TRUE(*verified);
}

} // namespace pbr
