// Envelope v4's frozen vectors (release-compatibility SIGNED_FORMATS § 2.1, § 6): the signed bytes of one fixed
// envelope with an empty extension area and with one unknown non-critical entry, and signed fixtures with the verdict
// the receiver's order gives each (accepted, older, needs update before verifying, needs update after it, forged,
// malformed). The committed file is data/envelope_vectors.json; PP_WRITE_ENVELOPE_VECTORS=<path> writes a fresh one
// (the device keys come from fixed seeds; ML-DSA signatures are randomized, so only they differ between runs).

#include "lib/delivery/EnvelopeSigner.h"

#include "common/chat/MessagingJson.h"
#include "common/tests/VectorFile.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/crypto/SignedReceive.h"
#include "foundation/crypto/TypedBlob.h"

#include "crypto/PeerId.h"

#include <gtest/gtest.h>

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

using ::pp::kMlDsa65SeedBytes;
using pp::common::Array;
using pp::common::Object;
using pp::common::Value;
using test::VectorFile;
using test::WithoutKeys;

const VectorFile kFile{PP_ENVELOPE_VECTORS_PATH, "PP_WRITE_ENVELOPE_VECTORS"};
const std::set<std::string> kSignatureKeys{"signature"};

constexpr uint16_t kUnknownNonCritical = 0x0101;
constexpr uint16_t kUnknownCritical = 0x8101;

MlDsaKeyPair KeysOfSeed(const uint8_t fill) {
  return MlDsa::GenerateKeyPairFromSeed(ByteVector(kMlDsa65SeedBytes, fill)).value();
}

/** The sending device (seed 0x41…) and another device (0x42…) that forges. */
const MlDsaKeyPair& Sender() {
  static const MlDsaKeyPair keys = KeysOfSeed(0x41);
  return keys;
}
const MlDsaKeyPair& Forger() {
  static const MlDsaKeyPair keys = KeysOfSeed(0x42);
  return keys;
}

std::string TypedB64(const pp::BlobKind kind, const size_t body_bytes, const uint8_t salt) {
  ByteVector body(body_bytes);
  for (size_t i = 0; i < body.size(); ++i) {
    body[i] = static_cast<uint8_t>((i * 7 + salt) & 0xff);
  }
  return Base64Encode(WriteTypedBlob(kind, body).value());
}

/** A blob of `kind` naming an algorithm id this build does not know (2). */
std::string UnknownAlgorithmB64(const uint8_t salt) {
  return Base64Encode(ByteVector{0x02, salt, 0x00, 0x01});
}

pp::ExtArea Area(const uint16_t type) {
  return pp::ExtArea::FromEntries({{.type = type, .value = {0x01, 0x02, 0x03}}}).value();
}

/** The fixed envelope: a direct e2e_public message, two copies (one opening its session with a key_init). */
RelayEnvelope FixedEnvelope() {
  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = "8d7f6e5c-4b3a-4291-8f0e-1d2c3b4a5968";
  envelope.sender_contact_id = "account:vector-sender";
  envelope.sender_device = pp::PeerIdFromMlDsaPublicKey(Sender().public_key).value();
  envelope.route.kind = kRelayRouteDirect;
  envelope.route.channel = ThreadChannel::E2ePublic;
  envelope.timestamp = 1'762'646'400'123;
  envelope.body.e2e.copies = {
      RelayCopy{.recipient_device = "QmRecipientPhone",
                .session_epoch = 3,
                .sender_seq = 17,
                .key_init_b64 = TypedB64(pp::BlobKind::kKemCiphertext, kHybridKemCiphertextBytes, 0x11),
                .payload_b64 = TypedB64(pp::BlobKind::kAeadSealed, 24 + 16 + 32, 0x22)},
      RelayCopy{.recipient_device = "QmRecipientLaptop",
                .session_epoch = 1,
                .sender_seq = 5,
                .payload_b64 = TypedB64(pp::BlobKind::kAeadSealed, 24 + 16 + 8, 0x33)},
  };
  return envelope;
}

RelayEnvelope SignedBy(RelayEnvelope envelope, const MlDsaKeyPair& keys) {
  EXPECT_TRUE(EnvelopeSigner::Sign(envelope, [&](const std::vector<uint8_t>& bytes) -> Roe<std::string> {
    auto signature = MlDsa::Sign(keys.secret_key, bytes);
    if (!signature) {
      return signature.error();
    }
    return Base64Encode(*signature);
  }));
  return envelope;
}

Object Case(const std::string& name, const Object& envelope) {
  Object row;
  row.set("name", name);
  row.set("envelope", envelope);
  return row;
}

Object SignBytesCase(const std::string& name, const RelayEnvelope& envelope) {
  Object row = Case(name, RelayEnvelopeToApplicationJson(envelope));
  row.set("sign_bytes_hex", BytesToHex(EnvelopeSigner::BuildSignBytes(envelope).value()));
  return row;
}

Object ReceiveCase(const std::string& name, const Object& envelope, const SignedVerdict verdict) {
  Object row = Case(name, envelope);
  row.set("verdict", std::string(SignedVerdictName(verdict)));
  return row;
}

Object Json(const RelayEnvelope& envelope) {
  return RelayEnvelopeToApplicationJson(envelope);
}

Object WithField(Object json, const std::string& key, Value value) {
  json.set(key, std::move(value));
  return json;
}

/** `json` with its first copy's `key` set to `value` (body.e2e.copies[0]). */
Object WithFirstCopyField(Object json, const std::string& key, const std::string& value) {
  Object body = *json.getObject("body");
  Object e2e = *body.getObject("e2e");
  std::vector<Value> copies = e2e.getArray("copies")->elements;
  Object first = *pp::common::asObject(copies.front());
  first.set(key, value);
  copies.front() = pp::common::ObjectValue(std::move(first));
  e2e.set("copies", pp::common::ArrayValue(std::move(copies)));
  body.set("e2e", e2e);
  json.set("body", body);
  return json;
}

Object BuildVectors() {
  const RelayEnvelope fixed = FixedEnvelope();
  RelayEnvelope with_entry = fixed;
  with_entry.ext = Area(kUnknownNonCritical);
  RelayEnvelope with_critical = fixed;
  with_critical.ext = Area(kUnknownCritical);
  const Object accepted = Json(SignedBy(fixed, Sender()));

  std::vector<Value> sign_bytes{
      pp::common::ObjectValue(SignBytesCase("empty_area", fixed)),
      pp::common::ObjectValue(SignBytesCase("one_unknown_non_critical_entry", with_entry)),
  };
  std::vector<Value> receive{
      pp::common::ObjectValue(ReceiveCase("signed", accepted, SignedVerdict::Accepted)),
      pp::common::ObjectValue(ReceiveCase("signed_with_an_unknown_non_critical_entry",
                                          Json(SignedBy(with_entry, Sender())), SignedVerdict::Accepted)),
      pp::common::ObjectValue(ReceiveCase(
          "version_plus_one", WithField(accepted, "envelope_version", Value(int64_t{kRelayEnvelopeVersion + 1})),
          SignedVerdict::NeedsUpdate)),
      pp::common::ObjectValue(ReceiveCase(
          "version_minus_one", WithField(accepted, "envelope_version", Value(int64_t{kRelayEnvelopeVersion - 1})),
          SignedVerdict::Older)),
      pp::common::ObjectValue(
          ReceiveCase("unknown_critical_entry", Json(SignedBy(with_critical, Sender())), SignedVerdict::NeedsUpdate)),
      pp::common::ObjectValue(ReceiveCase("unknown_signature_algorithm",
                                          WithField(accepted, "signature", Value(UnknownAlgorithmB64(0x01))),
                                          SignedVerdict::NeedsUpdate)),
      pp::common::ObjectValue(ReceiveCase("unknown_kem_algorithm",
                                          WithFirstCopyField(accepted, "key_init_b64", UnknownAlgorithmB64(0x02)),
                                          SignedVerdict::NeedsUpdate)),
      pp::common::ObjectValue(ReceiveCase("unknown_aead_algorithm",
                                          WithFirstCopyField(accepted, "payload_b64", UnknownAlgorithmB64(0x03)),
                                          SignedVerdict::NeedsUpdate)),
      pp::common::ObjectValue(
          ReceiveCase("signed_by_another_key", Json(SignedBy(fixed, Forger())), SignedVerdict::Forged)),
      pp::common::ObjectValue(
          ReceiveCase("an_unsigned_critical_entry",
                      WithField(accepted, kExtB64Key, Value(Base64Encode(Area(kUnknownCritical).EncodeEntries()))),
                      SignedVerdict::Forged)),
      pp::common::ObjectValue(ReceiveCase("malformed_extension_area",
                                          WithField(accepted, kExtB64Key, Value(std::string("AAAA"))),
                                          SignedVerdict::Malformed)),
  };

  Object vectors;
  vectors.set("format", "pp-browser envelope vectors v1");
  vectors.set("about",
              "Envelope v4 (release-compatibility SIGNED_FORMATS 2.1): sign_bytes are frozen (hex); receive fixtures "
              "give the receiver's verdict (SIGNED_FORMATS 1.1). Signatures are randomized: compare without them.");
  vectors.set("domain", EnvelopeSigner::kDomain);
  vectors.set("envelope_version", int64_t{kRelayEnvelopeVersion});
  vectors.set("sending_device_seed_hex", BytesToHex(ByteVector(kMlDsa65SeedBytes, 0x41)));
  vectors.set("sending_device_public_key_b64",
              Base64Encode(TypedSigningPublicKey(Sender().public_key).value())); // typed, as a v5 card carries it
  vectors.set("sign_bytes", pp::common::ArrayValue(std::move(sign_bytes)));
  vectors.set("receive", pp::common::ArrayValue(std::move(receive)));
  return vectors;
}

/** The receiver's verdict on one envelope JSON: ParseRelayEnvelope (steps 1–3), then EnvelopeSigner::Receive. */
SignedVerdict VerdictOf(const Object& json, const std::string& device_key_b64) {
  auto parsed = ParseRelayEnvelope(json);
  if (!parsed) {
    // Refused before verifying: an older app's when its outside version is, else malformed.
    const auto version = OutsideVersionOfJson(json, "envelope_version");
    return !version || *version < kRelayEnvelopeVersion ? SignedVerdict::Older : SignedVerdict::Malformed;
  }
  if (RelayEnvelopeNeedsUpdate(*parsed)) {
    return SignedVerdict::NeedsUpdate; // a newer app's (T2): nothing verified
  }
  return EnvelopeSigner::Receive(*parsed, device_key_b64).verdict;
}

TEST(EnvelopeVectorsTest, WriteAFreshFileWhenAsked) {
  if (!kFile.WriteRequested()) {
    GTEST_SKIP() << kFile.SkipReason();
  }
  ASSERT_TRUE(kFile.Write(BuildVectors()));
}

TEST(EnvelopeVectorsTest, TheCommittedFileIsWhatTheBuildersMake) {
  EXPECT_EQ(VectorFile::Text(WithoutKeys(kFile.Read(), kSignatureKeys)),
            VectorFile::Text(WithoutKeys(BuildVectors(), kSignatureKeys)))
      << kFile.RegenerateHint();
}

// The frozen bytes, rebuilt from the committed JSON: a change to what is signed changes them, and fails here.
TEST(EnvelopeVectorsTest, TheCommittedSignBytesRebuildFromTheirJson) {
  const Object file = kFile.Read();
  const Array* cases = file.getArray("sign_bytes");
  ASSERT_TRUE(cases != nullptr);
  ASSERT_EQ(cases->elements.size(), 2u);
  for (const Value& item : cases->elements) {
    const Object& row = *pp::common::asObject(item);
    const std::string name = row.getString("name").value_or("");
    auto envelope = ParseRelayEnvelope(*row.getObject("envelope"));
    ASSERT_TRUE(envelope) << name << ": " << envelope.error().message;
    auto bytes = EnvelopeSigner::BuildSignBytes(*envelope);
    ASSERT_TRUE(bytes) << name;
    EXPECT_EQ(BytesToHex(*bytes), row.getString("sign_bytes_hex").value_or("")) << name;
    EXPECT_EQ(SignedBytesVersion(*bytes), std::optional<uint8_t>(static_cast<uint8_t>(kRelayEnvelopeVersion))) << name;
    // The area is kept as it came: written back, the JSON's ext_b64 is the one read.
    EXPECT_EQ(Json(*envelope).getString(kExtB64Key), row.getObject("envelope")->getString(kExtB64Key)) << name;
  }
}

// Every committed fixture gets its verdict: the signed ones verify with the committed key (signatures are not rebuilt).
TEST(EnvelopeVectorsTest, EachCommittedFixtureGetsItsVerdict) {
  const Object file = kFile.Read();
  const std::string key =
      file.getString("sending_device_public_key_b64").value_or(""); // held typed, as the card holds it
  const Array* cases = file.getArray("receive");
  ASSERT_TRUE(cases != nullptr);
  ASSERT_EQ(cases->elements.size(), 11u);
  for (const Value& item : cases->elements) {
    const Object& row = *pp::common::asObject(item);
    const std::string name = row.getString("name").value_or("");
    EXPECT_EQ(std::string(SignedVerdictName(VerdictOf(*row.getObject("envelope"), key))),
              row.getString("verdict").value_or(""))
        << name;
  }
}

} // namespace
} // namespace pbr
