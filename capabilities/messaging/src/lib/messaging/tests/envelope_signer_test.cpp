#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/crypto/TypedBlob.h"
#include "lib/delivery/EnvelopeSigner.h"
#include "lib/people/PeerSigningKeyStore.h"
#include "common/chat/MessagingJson.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

RelayEnvelope MakeE2eVectorEnvelope() {
  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = "660e8400-e29b-41d4-a716-446655440001";
  envelope.sender_contact_id = "account:alice123";
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

RawSignFn RawSigner(const MlDsaKeyPair& keys) {
  return [&keys](const std::vector<uint8_t>& bytes) -> Roe<std::string> {
    auto signature = MlDsa::Sign(keys.secret_key, bytes);
    if (!signature) {
      return signature.error();
    }
    return Base64Encode(*signature);
  };
}

const MlDsaKeyPair& DeviceKeys() {
  static const MlDsaKeyPair keys = MlDsa::GenerateKeyPair().value();
  return keys;
}

/** A device key as its account card lists it: typed, base64. */
std::string TypedKey(const MlDsaKeyPair& keys) {
  return WriteTypedBlobB64(pp::BlobKind::kSignaturePublicKey, keys.public_key).value();
}

pp::ExtArea Area(const uint16_t type) {
  return pp::ExtArea::FromEntries({{.type = type, .value = {9}}}).value();
}

} // namespace

TEST(EnvelopeSignerTest, CopyHashMatchesFrozenVector) {
  const RelayEnvelope envelope = MakeE2eVectorEnvelope();
  auto body_hash = EnvelopeSigner::CopyHash(envelope.body.e2e.copies.front());
  ASSERT_TRUE(body_hash);
  EXPECT_EQ(BytesToHex(*body_hash), "f046c5fdbe2e312864d2dc1386e8c63eda7062f3ce2a2c1abed3860a1a566f3c");
}

// The signed bytes, frozen: every signed field set to a fixed value (two copies, a key_init, an e2e_public route).
// A receiver checks `envelope_version` before it verifies, so changing what is signed — the sign domain or version,
// a field covered, its order or encoding — without bumping a version the receiver checks makes every older peer see
// forgeries instead of "update" (docs/contracts/COMPATIBILITY.md, "Changing what is signed"; #555). If this hash
// changes, bump such a version in the same change and update both expectations here (and envelope_vectors.json).
TEST(EnvelopeSignerTest, SignBytesMatchFrozenHash) {
  EXPECT_EQ(kRelayEnvelopeVersion, 4) << "the version a receiver checks before it verifies";
  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = "8d7f6e5c-4b3a-4291-8f0e-1d2c3b4a5968";
  envelope.sender_contact_id = "account:alice";
  envelope.sender_device = "QmAliceDevice";
  envelope.route.kind = kRelayRouteDirect;
  envelope.route.channel = ThreadChannel::E2ePublic;
  envelope.timestamp = 1762646400123;
  envelope.body.e2e.copies = {
      RelayCopy{.recipient_device = "QmBobPhone",
                .session_epoch = 3,
                .sender_seq = 17,
                .key_init_b64 = "AAECAwQFBgcICQoLDA0ODw==",
                .payload_b64 = "EBESExQVFhcYGRobHB0eHw=="},
      RelayCopy{.recipient_device = "QmBobLaptop", .session_epoch = 1, .sender_seq = 5, .payload_b64 = "ICEiIyQlJico"},
  };
  auto bytes = EnvelopeSigner::BuildSignBytes(envelope);
  ASSERT_TRUE(bytes) << bytes.error().message;
  EXPECT_EQ(BytesToHex(Blake2b256(*bytes)), "3654d3876dcd1aa9a781b1ec617912f0ba0dff17bbac8f7a836b49ee0b1bff45");
}

// "pp-browser:relay-envelope" (25) 00 ‖ u8 version ‖ u8 route_kind ‖ u8 channel: the route kind and channel at 27, 28.
TEST(EnvelopeSignerTest, BuildSignBytesUsesE2eChannelZero) {
  const RelayEnvelope envelope = MakeE2eVectorEnvelope();
  auto sign_bytes = EnvelopeSigner::BuildSignBytes(envelope);
  ASSERT_TRUE(sign_bytes);
  ASSERT_GE(sign_bytes->size(), 29u);
  EXPECT_EQ((*sign_bytes)[27], 0u);
  EXPECT_EQ((*sign_bytes)[28], 0u);
}

TEST(EnvelopeSignerTest, SignAndReceiveRoundTrip) {
  RelayEnvelope envelope = MakeE2eVectorEnvelope();
  envelope.message_id = "770e8400-e29b-41d4-a716-446655440002";
  ASSERT_TRUE(EnvelopeSigner::Sign(envelope, RawSigner(DeviceKeys())));
  auto signature = Base64Decode(envelope.signature);
  ASSERT_TRUE(signature);
  EXPECT_EQ(signature->size(), 3310u) << "a typed signature";
  EXPECT_EQ(signature->front(), 1u) << "ml-dsa-65";

  const SignedReceipt receipt = EnvelopeSigner::Receive(envelope, TypedKey(DeviceKeys()));
  EXPECT_TRUE(receipt.accepted()) << receipt.detail;
}

// The receiver's order (SIGNED_FORMATS § 1.1) for an envelope: each refusal its own verdict, "update needed" never a
// forgery, the extension area read only after the signature.
TEST(EnvelopeSignerTest, ReceiveRunsTheReceiversOrder) {
  const std::string key = TypedKey(DeviceKeys());
  RelayEnvelope envelope = MakeE2eVectorEnvelope();
  ASSERT_TRUE(EnvelopeSigner::Sign(envelope, RawSigner(DeviceKeys())));
  ASSERT_TRUE(EnvelopeSigner::Receive(envelope, key).accepted());

  RelayEnvelope older = envelope;
  older.envelope_version = kRelayEnvelopeVersion - 1;
  EXPECT_EQ(EnvelopeSigner::Receive(older, key).verdict, SignedVerdict::Older);
  RelayEnvelope newer = envelope;
  newer.envelope_version = kRelayEnvelopeVersion + 1;
  EXPECT_EQ(EnvelopeSigner::Receive(newer, key).verdict, SignedVerdict::NeedsUpdate);

  RelayEnvelope unsigned_envelope = envelope;
  unsigned_envelope.signature.clear();
  EXPECT_EQ(EnvelopeSigner::Receive(unsigned_envelope, key).verdict, SignedVerdict::Malformed);
  RelayEnvelope short_signature = envelope;
  short_signature.signature = Base64Encode(ByteVector{1, 2, 3});
  EXPECT_EQ(EnvelopeSigner::Receive(short_signature, key).verdict, SignedVerdict::Malformed);
  RelayEnvelope unknown_algorithm = envelope;
  unknown_algorithm.signature = Base64Encode(ByteVector{2, 2, 3});
  EXPECT_EQ(EnvelopeSigner::Receive(unknown_algorithm, key).verdict, SignedVerdict::NeedsUpdate);

  RelayEnvelope changed = envelope;
  changed.timestamp += 1;
  EXPECT_EQ(EnvelopeSigner::Receive(changed, key).verdict, SignedVerdict::Forged);
  auto other = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(other);
  EXPECT_EQ(EnvelopeSigner::Receive(envelope, TypedKey(*other)).verdict, SignedVerdict::Forged);

  // An unknown critical entry: update needed once signed; a forgery when added after signing.
  RelayEnvelope critical = envelope;
  critical.ext = Area(0x8001);
  EXPECT_EQ(EnvelopeSigner::Receive(critical, key).verdict, SignedVerdict::Forged);
  ASSERT_TRUE(EnvelopeSigner::Sign(critical, RawSigner(DeviceKeys())));
  EXPECT_EQ(EnvelopeSigner::Receive(critical, key).verdict, SignedVerdict::NeedsUpdate);
  // A non-critical one is ignored, and it is signed.
  RelayEnvelope non_critical = envelope;
  non_critical.ext = Area(0x0001);
  EXPECT_EQ(EnvelopeSigner::Receive(non_critical, key).verdict, SignedVerdict::Forged);
  ASSERT_TRUE(EnvelopeSigner::Sign(non_critical, RawSigner(DeviceKeys())));
  EXPECT_TRUE(EnvelopeSigner::Receive(non_critical, key).accepted());
}

// Everything that decides something on receipt is signed — the decrypted blob (and, inside it, a group copy's
// group), the AutoKey key_init, the sending account and the sending and recipient devices (v3).
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
  changed.sender_contact_id = "account:someone-else";
  EXPECT_TRUE(differs(changed)) << "the sending account";
  changed = base;
  changed.sender_device = "QmOtherDevice";
  EXPECT_TRUE(differs(changed)) << "the sending device";
  changed = base;
  changed.ext = Area(0x0001);
  EXPECT_TRUE(differs(changed)) << "the extension area";

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
  auto group_bytes = EnvelopeSigner::BuildSignBytes(group);
  ASSERT_TRUE(group_bytes) << group_bytes.error().message;
  EXPECT_NE(*group_bytes, *base_bytes) << "the route kind";
}

// Envelope v4 (SIGNED_FORMATS § 2.1): one version, after a domain without -vN; the bytes name no relay id (MR001) nor
// group (G018 item 7) — a group copy carries its group id inside its ciphertext, bound there by the AEAD and by the
// copy hash these bytes sign. The extension area is the last field (empty: its u64 length 0).
TEST(EnvelopeSignerTest, SignBytesAreV4AndNameNoGroupNorRelayId) {
  RelayEnvelope group = MakeE2eVectorEnvelope();
  group.route.kind = kRelayRouteGroup;
  auto bytes = EnvelopeSigner::BuildSignBytes(group);
  ASSERT_TRUE(bytes) << bytes.error().message;
  const std::string domain = "pp-browser:relay-envelope";
  ASSERT_GE(bytes->size(), domain.size() + 6);
  EXPECT_TRUE(IsSignedDomain(domain));
  EXPECT_EQ(std::string(bytes->begin(), bytes->begin() + static_cast<std::ptrdiff_t>(domain.size())), domain);
  EXPECT_EQ((*bytes)[domain.size()], 0u) << "NUL after the domain";
  EXPECT_EQ((*bytes)[26], 4u) << "envelope_version, the only version byte";
  EXPECT_EQ((*bytes)[27], 1u) << "route kind group";
  EXPECT_EQ((*bytes)[28], 0xFFu) << "a group has no channel";
  EXPECT_EQ(std::vector<uint8_t>(bytes->end() - 8, bytes->end()), std::vector<uint8_t>(8, 0)) << "an empty area";
  const auto len_utf8 = [](const std::string& value) {
    std::vector<uint8_t> out(8, 0);
    const uint64_t size = value.size();
    for (int i = 0; i < 8; ++i) {
      out[7 - i] = static_cast<uint8_t>(size >> (8 * i));
    }
    out.insert(out.end(), value.begin(), value.end());
    return out;
  };
  std::vector<uint8_t> expected;
  for (const std::string* field : {&group.message_id, &group.sender_contact_id, &group.sender_device}) {
    const std::vector<uint8_t> encoded = len_utf8(*field);
    expected.insert(expected.end(), encoded.begin(), encoded.end());
  }
  // After the timestamp (29..36): message_id, sender_contact_id, then straight to sender_device.
  ASSERT_GE(bytes->size(), 37 + expected.size());
  EXPECT_EQ(
      std::vector<uint8_t>(bytes->begin() + 37, bytes->begin() + 37 + static_cast<std::ptrdiff_t>(expected.size())),
      expected)
      << "no group id (G018) nor relay id (MR001) between the sender's account and its device";
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
  EXPECT_TRUE(EnvelopeSigner::BuildSignBytes(control)) << "the control lane: the account copy";
  control.body.e2e.copies.front().recipient_device = "QmDevice";
  EXPECT_TRUE(EnvelopeSigner::BuildSignBytes(control)) << "or a copy per device of the peer's (M024)";
  RelayEnvelope control_twice = control;
  control_twice.body.e2e.copies.push_back(control.body.e2e.copies.front());
  EXPECT_FALSE(EnvelopeSigner::BuildSignBytes(control_twice)) << "never two copies for one device";

  RelayEnvelope group = base;
  group.route.kind = kRelayRouteGroup;
  EXPECT_TRUE(EnvelopeSigner::BuildSignBytes(group)) << "a group envelope names no group (G018 item 7)";

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
  record.signing_public_key_b64 = WriteTypedBlobB64(pp::BlobKind::kSignaturePublicKey, mine->public_key).value();
  record.source = "test";

  PeerSigningKeyStore store;
  EXPECT_FALSE(store.Put("account", *other_account, record)) << "another account";
  EXPECT_FALSE(store.Get("account", *other_account));
  EXPECT_FALSE(store.Put("custom", "custom:me", record)) << "not self-certifying";
  EXPECT_FALSE(store.Put("peer_id", "QmNotMine", record)) << "a PeerId it does not derive";
  EXPECT_TRUE(store.Put("account", *my_account, record));
  ASSERT_TRUE(store.Get("account", *my_account));
}

} // namespace pbr
