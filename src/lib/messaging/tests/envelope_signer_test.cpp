#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "lib/messaging/EnvelopeSigner.h"
#include "lib/messaging/PeerSigningKeyStore.h"
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
  envelope.sender_device = "QmAliceDevice";
  envelope.route.kind = "direct";
  envelope.route.channel = ThreadChannel::E2e;
  envelope.timestamp = 1719662400456;
  return WithSingleRelayCopy(
      std::move(envelope),
      RelayCopy{.session_epoch = 1,
                .sender_seq = 42,
                .payload_b64 = "AQABAgMEBQYHCAkKCwwNDg8QERITFBUWF1vPScnCPAVe+dnJiV9kKBztMM3qj/Hi+RhfLy6wlhU="});
}

} // namespace

TEST(EnvelopeSignerTest, CopyHashMatchesFrozenVector) {
  const RelayEnvelope envelope = MakeE2eVectorEnvelope();
  auto body_hash = EnvelopeSigner::CopyHash(envelope.body.e2e.copies.front());
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

// Everything that decides something on receipt is signed — the decrypted blob, the group, the AutoKey
// key_init, the sender's relay id (v2) and the sending and recipient devices (v3).
TEST(EnvelopeSignerTest, SignBytesCoverEveryFieldTheReceiverActsOn) {
  const RelayEnvelope base = MakeE2eVectorEnvelope();
  auto base_bytes = EnvelopeSigner::BuildSignBytes(base);
  ASSERT_TRUE(base_bytes);
  const auto differs = [&](const RelayEnvelope& changed) {
    auto bytes = EnvelopeSigner::BuildSignBytes(changed);
    return !bytes || *bytes != *base_bytes;
  };
  const auto with_copy = [&](RelayCopy copy) {
    RelayEnvelope changed = base;
    changed.body.e2e.copies = {copy};
    return changed;
  };
  const RelayCopy copy = base.body.e2e.copies.front();
  RelayCopy other = copy;
  other.payload_b64 = "AQID";
  EXPECT_TRUE(differs(with_copy(other))) << "payload";
  other = copy;
  other.key_init_b64 = "AQID";
  EXPECT_TRUE(differs(with_copy(other))) << "key_init";
  other = copy;
  other.sender_seq = 43;
  EXPECT_TRUE(differs(with_copy(other))) << "the copy's seq";
  other = copy;
  other.session_epoch = 2;
  EXPECT_TRUE(differs(with_copy(other))) << "the copy's epoch";
  other = copy;
  other.recipient_device = "QmSomeDevice";
  EXPECT_TRUE(differs(with_copy(other))) << "the device the copy is for (M022)";
  RelayEnvelope changed = base;
  changed.sender_relay_id = "relay:other";
  EXPECT_TRUE(differs(changed)) << "sender_relay_id";
  changed = base;
  changed.sender_device = "QmOtherDevice";
  EXPECT_TRUE(differs(changed)) << "the sending device";

  // Every copy of the envelope is signed, in device order whatever the list's order.
  RelayCopy phone = copy;
  phone.recipient_device = "QmPhone";
  RelayCopy laptop = copy;
  laptop.recipient_device = "QmLaptop";
  laptop.sender_seq = 7;
  RelayEnvelope both = base;
  both.body.e2e.copies = {phone, laptop};
  RelayEnvelope reordered = base;
  reordered.body.e2e.copies = {laptop, phone};
  EXPECT_EQ(EnvelopeSigner::BuildSignBytes(both).value(), EnvelopeSigner::BuildSignBytes(reordered).value());
  RelayEnvelope one_dropped = base;
  one_dropped.body.e2e.copies = {phone};
  EXPECT_TRUE(EnvelopeSigner::BuildSignBytes(one_dropped).value() != EnvelopeSigner::BuildSignBytes(both).value())
      << "a copy taken out breaks the signature";

  RelayEnvelope group = base;
  group.route.kind = kRelayRouteGroup;
  group.route.group_id = "group:a";
  auto group_bytes = EnvelopeSigner::BuildSignBytes(group);
  ASSERT_TRUE(group_bytes) << group_bytes.error().message;
  RelayEnvelope other_group = group;
  other_group.route.group_id = "group:b";
  auto other_bytes = EnvelopeSigner::BuildSignBytes(other_group);
  ASSERT_TRUE(other_bytes);
  EXPECT_NE(*group_bytes, *other_bytes) << "group_id";
}

// One body shape per route: the signer refuses (and so verification fails on) anything else.
TEST(EnvelopeSignerTest, MalformedEnvelopesAreRefused) {
  const RelayEnvelope base = MakeE2eVectorEnvelope();
  RelayEnvelope none = base;
  none.body.e2e.copies.clear();
  none.body.e2e.payload_b64.clear();
  EXPECT_FALSE(EnvelopeSigner::BuildSignBytes(none)) << "no copy";
  RelayEnvelope twice = base;
  twice.body.e2e.copies = {base.body.e2e.copies.front(), base.body.e2e.copies.front()};
  EXPECT_FALSE(EnvelopeSigner::BuildSignBytes(twice)) << "two copies for one device";
  RelayEnvelope empty_payload = base;
  empty_payload.body.e2e.copies = {RelayCopy{.recipient_device = "QmDevice", .session_epoch = 1}};
  EXPECT_FALSE(EnvelopeSigner::BuildSignBytes(empty_payload)) << "a copy without a payload";

  RelayEnvelope control = base;
  control.route.kind = kRelayRouteControl;
  EXPECT_TRUE(EnvelopeSigner::BuildSignBytes(control)) << "the control lane: one account copy";
  control.body.e2e.copies.front().recipient_device = "QmDevice";
  EXPECT_FALSE(EnvelopeSigner::BuildSignBytes(control)) << "a control envelope for one device";

  RelayEnvelope group = base;
  group.route.kind = kRelayRouteGroup;
  EXPECT_FALSE(EnvelopeSigner::BuildSignBytes(group)) << "group without group_id";
  RelayEnvelope direct = base;
  direct.route.group_id = "group:a";
  EXPECT_FALSE(EnvelopeSigner::BuildSignBytes(direct)) << "direct with a group_id";

  RelayEnvelope unknown = base;
  unknown.route.kind = "other";
  EXPECT_FALSE(EnvelopeSigner::BuildSignBytes(unknown)) << "unknown route kind";
}

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
