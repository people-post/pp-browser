#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "domain/messaging/EnvelopeSigner.h"
#include "domain/messaging/PeerSigningKeyStore.h"
#include "common/chat/MessagingJson.h"

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
  auto body_hash = EnvelopeSigner::BodyHash(envelope);
  ASSERT_TRUE(body_hash);
  EXPECT_EQ(BytesToHex(*body_hash), "f046c5fdbe2e312864d2dc1386e8c63eda7062f3ce2a2c1abed3860a1a566f3c");
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

// v2: everything that decides something on receipt is signed — the decrypted blob, the group,
// the AutoKey key_init and the sender's relay id.
TEST(EnvelopeSignerTest, SignBytesCoverEveryFieldTheReceiverActsOn) {
  const RelayEnvelope base = MakeE2eVectorEnvelope();
  auto base_bytes = EnvelopeSigner::BuildSignBytes(base);
  ASSERT_TRUE(base_bytes);
  const auto differs = [&](const RelayEnvelope& changed) {
    auto bytes = EnvelopeSigner::BuildSignBytes(changed);
    return !bytes || *bytes != *base_bytes;
  };
  RelayEnvelope changed = base;
  changed.body.e2e.payload_b64 = "AQID";
  EXPECT_TRUE(differs(changed)) << "payload";
  changed = base;
  changed.body.e2e.key_init_b64 = "AQID";
  EXPECT_TRUE(differs(changed)) << "key_init";
  changed = base;
  changed.sender_relay_id = "relay:other";
  EXPECT_TRUE(differs(changed)) << "sender_relay_id";

  RelayEnvelope group = base;
  group.route.kind = kRelayRouteGroup;
  group.route.group_id = "group:a";
  group.body.e2e.member_payloads = std::map<std::string, std::string>{{"account:bob", base.body.e2e.payload_b64}};
  group.body.e2e.payload_b64.clear();
  auto group_bytes = EnvelopeSigner::BuildSignBytes(group);
  ASSERT_TRUE(group_bytes) << group_bytes.error().message;
  RelayEnvelope other_group = group;
  other_group.route.group_id = "group:b";
  auto other_bytes = EnvelopeSigner::BuildSignBytes(other_group);
  ASSERT_TRUE(other_bytes);
  EXPECT_NE(*group_bytes, *other_bytes) << "group_id";
  RelayEnvelope other_member = group;
  other_member.body.e2e.member_payloads = std::map<std::string, std::string>{{"account:carol", base.body.e2e.payload_b64}};
  auto member_bytes = EnvelopeSigner::BuildSignBytes(other_member);
  ASSERT_TRUE(member_bytes);
  EXPECT_NE(*group_bytes, *member_bytes) << "member id";
}

// One body shape per route: the signer refuses (and so verification fails on) anything else.
TEST(EnvelopeSignerTest, MixedBodyShapesAreRefused) {
  RelayEnvelope direct = MakeE2eVectorEnvelope();
  direct.body.e2e.member_payloads = std::map<std::string, std::string>{{"x", direct.body.e2e.payload_b64}};
  EXPECT_FALSE(EnvelopeSigner::BuildSignBytes(direct)) << "direct with member_payloads";

  RelayEnvelope control = MakeE2eVectorEnvelope();
  control.route.kind = kRelayRouteControl;
  control.body.e2e.member_payloads = std::map<std::string, std::string>{{"x", control.body.e2e.payload_b64}};
  EXPECT_FALSE(EnvelopeSigner::BuildSignBytes(control)) << "control with member_payloads";

  RelayEnvelope group = MakeE2eVectorEnvelope();
  group.route.kind = kRelayRouteGroup;
  group.route.group_id = "group:a";
  group.body.e2e.member_payloads = std::map<std::string, std::string>{{"x", group.body.e2e.payload_b64}};
  EXPECT_FALSE(EnvelopeSigner::BuildSignBytes(group)) << "group with payload_b64";

  RelayEnvelope unknown = MakeE2eVectorEnvelope();
  unknown.route.kind = "other";
  EXPECT_FALSE(EnvelopeSigner::BuildSignBytes(unknown)) << "unknown route kind";
}

// An account's signing key must hash to the account (self-certifying Account ID): a key filed under
// another account, or under another identity kind, is refused whoever offers it.
TEST(PeerSigningKeyStoreTest, AKeyIsKeptOnlyUnderTheAccountItHashesTo) {
  auto mine = MlDsa::GenerateKeyPair();
  auto other = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(mine && other);
  auto my_account = AccountIdFromMlDsaPublicKey(mine->public_key);
  auto other_account = AccountIdFromMlDsaPublicKey(other->public_key);
  ASSERT_TRUE(my_account && other_account);
  PeerSigningKeyRecord record;
  record.signing_public_key_b64 = Base64Encode(mine->public_key);
  record.source = "test";

  PeerSigningKeyStore store;
  EXPECT_FALSE(store.Put("account", *other_account, record)) << "another account";
  EXPECT_FALSE(store.Get("account", *other_account));
  EXPECT_FALSE(store.Put("relay_user", "relay:me", record)) << "not self-certifying";
  EXPECT_FALSE(store.Put("peer_id", "QmNotMine", record)) << "a PeerId it does not derive";
  EXPECT_TRUE(store.Put("account", *my_account, record));
  ASSERT_TRUE(store.Get("account", *my_account));
}

} // namespace pbr
