// Relay grant v2's frozen vectors (release-compatibility SIGNED_FORMATS § 2.4, § 6): the signed bytes of fixed grants
// (an empty area, one unknown non-critical entry), signed fixtures, and the receiver's verdict on each refusal. The
// relay's grant check (pp-relay-apis `grant.ts`) reads data/relay_grant_vectors.json byte for byte.
// PP_WRITE_RELAY_GRANT_VECTORS=<path> writes a fresh file; the comparison leaves out the hedged signatures.

#include "lib/delivery/RelayGrantSigner.h"

#include "common/ValueJson.h"
#include "common/tests/VectorFile.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/crypto/TypedBlob.h"
#include "foundation/error/AppError.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

namespace pbr {
namespace {

using pp::ExtArea;
using pp::common::Array;
using pp::common::ArrayValue;
using pp::common::ObjectValue;
using pp::common::Value;
using test::VectorFile;
using test::WithoutKeys;

const VectorFile kFile{PP_RELAY_GRANT_VECTORS_PATH, "PP_WRITE_RELAY_GRANT_VECTORS"};
const std::set<std::string> kSignatureKeys = {"signature"};

constexpr char kFormat[] = "pp-browser relay grant vectors v1";
constexpr int64_t kIssuedAt = 1761235200000; // 2025-10-23T16:00:00Z
constexpr int64_t kDayMs = 24LL * 60 * 60 * 1000;

struct Keys {
  MlDsaKeyPair recipient;
  MlDsaKeyPair other;
  std::string recipient_id;
  std::string sender_id;
};

const Keys& VectorKeys() {
  static const Keys keys = [] {
    Keys k;
    k.recipient = MlDsa::GenerateKeyPairFromSeed(ByteVector(pp::kMlDsa65SeedBytes, 0x01)).value();
    const MlDsaKeyPair sender = MlDsa::GenerateKeyPairFromSeed(ByteVector(pp::kMlDsa65SeedBytes, 0x02)).value();
    k.other = MlDsa::GenerateKeyPairFromSeed(ByteVector(pp::kMlDsa65SeedBytes, 0x03)).value();
    k.recipient_id = AccountIdFromMlDsaPublicKey(k.recipient.public_key).value();
    k.sender_id = AccountIdFromMlDsaPublicKey(sender.public_key).value();
    return k;
  }();
  return keys;
}

ExtArea OneEntry(const uint16_t type) {
  return ExtArea::FromEntries({{.type = type, .value = {0xca, 0xfe}}}).value();
}

RelayGrant FixedGrant(const ExtArea& ext) {
  const Keys& keys = VectorKeys();
  return RelayGrant{.recipient_account_id = keys.recipient_id,
                    .sender_account_id = keys.sender_id,
                    .issued_at = kIssuedAt,
                    .expires_at = kIssuedAt + 30 * kDayMs,
                    .ext = ext};
}

/** A grant's JSON signed by `secret_key` over its v2 bytes. */
Object Signed(const RelayGrant& grant, const ByteVector& secret_key) {
  RelayGrant out = grant;
  out.signature = Base64Encode(SignTyped(secret_key, BuildRelayGrantSignBytes(grant)).value());
  return RelayGrantToJson(out);
}

Object SignBytesCase(const std::string& name, const RelayGrant& grant) {
  Object row;
  row.set("name", name);
  Object fields = RelayGrantToJson(grant);
  fields.erase("signature");
  row.set("grant", ObjectValue(fields));
  row.set("sign_bytes_hex", BytesToHex(BuildRelayGrantSignBytes(grant)));
  return row;
}

Object Fixture(const std::string& name, const Object& grant, const SignedVerdict verdict, const std::string& why) {
  Object row;
  row.set("name", name);
  row.set("grant", ObjectValue(grant));
  row.set("verdict", std::string(SignedVerdictName(verdict)));
  row.set("why", why);
  return row;
}

Object BuildVectors() {
  const Keys& keys = VectorKeys();
  const Object valid = Signed(FixedGrant(ExtArea{}), keys.recipient.secret_key);

  std::vector<Value> fixtures;
  fixtures.push_back(ObjectValue(Fixture("valid", valid, SignedVerdict::Accepted, "signed by the recipient")));
  fixtures.push_back(
      ObjectValue(Fixture("unknown_non_critical_entry", Signed(FixedGrant(OneEntry(0x0042)), keys.recipient.secret_key),
                          SignedVerdict::Accepted, "ignored and kept: ext_b64 is stored as received")));
  Object newer = valid;
  newer.set(kRelayGrantVersionKey, int64_t{3});
  newer.set("signature", std::string("not even base64"));
  fixtures.push_back(ObjectValue(
      Fixture("grant_version_3", newer, SignedVerdict::NeedsUpdate, "read first: nothing else is read or verified")));
  Object older = valid;
  older.set(kRelayGrantVersionKey, int64_t{1});
  fixtures.push_back(ObjectValue(Fixture("grant_version_1", older, SignedVerdict::Older, "below the floor")));
  Object missing = valid;
  missing.erase(kRelayGrantVersionKey);
  fixtures.push_back(ObjectValue(Fixture("no_grant_version", missing, SignedVerdict::Older, "made by an older app")));
  fixtures.push_back(
      ObjectValue(Fixture("unknown_critical_entry", Signed(FixedGrant(OneEntry(0x8042)), keys.recipient.secret_key),
                          SignedVerdict::NeedsUpdate, "it verifies, then the entry is read")));
  Object unknown_alg = valid;
  ByteVector signature = Base64Decode(valid.getString("signature").value()).value();
  signature[0] = 2;
  unknown_alg.set("signature", Base64Encode(signature));
  fixtures.push_back(ObjectValue(Fixture("unknown_signature_algorithm", unknown_alg, SignedVerdict::NeedsUpdate,
                                         "signature algorithm id 2: not verified")));
  Object added_area = valid;
  added_area.set(kExtB64Key, Base64Encode(OneEntry(0x8042).EncodeEntries()));
  fixtures.push_back(ObjectValue(Fixture("unsigned_area_added", added_area, SignedVerdict::Forged,
                                         "the area is in the signed bytes: an unsigned one never says update")));
  Object bad_area = valid;
  bad_area.set(kExtB64Key, std::string("AAAA"));
  fixtures.push_back(
      ObjectValue(Fixture("malformed_area", bad_area, SignedVerdict::Malformed, "three bytes: no entry")));
  Object edited = valid;
  edited.set("expires_at", kIssuedAt + 30 * kDayMs - 1);
  fixtures.push_back(
      ObjectValue(Fixture("expires_at_edited", edited, SignedVerdict::Forged, "the signature covers it")));
  fixtures.push_back(ObjectValue(Fixture("signed_by_another_key", Signed(FixedGrant(ExtArea{}), keys.other.secret_key),
                                         SignedVerdict::Forged, "not the recipient's key")));

  Object root;
  root.set("format", std::string(kFormat));
  root.set("domain", std::string(kRelayGrantDomain));
  root.set(kRelayGrantVersionKey, int64_t{kRelayGrantVersion});
  root.set("layout", std::string("domain 00 u8(2) LenUtf8(recipient_account_id) LenUtf8(sender_account_id) "
                                 "i64(issued_at) i64(expires_at) u64(len(entries)) entries"));
  root.set("recipient_public_key_b64", Base64Encode(TypedSigningPublicKey(keys.recipient.public_key).value()));
  root.set("sign_bytes",
           ArrayValue({ObjectValue(SignBytesCase("empty_area", FixedGrant(ExtArea{}))),
                       ObjectValue(SignBytesCase("unknown_non_critical_entry", FixedGrant(OneEntry(0x0042))))}));
  root.set("fixtures", ArrayValue(std::move(fixtures)));
  return root;
}

TEST(RelayGrantVectorsTest, WriteAFreshFileWhenAsked) {
  if (!kFile.WriteRequested()) {
    GTEST_SKIP() << kFile.SkipReason();
  }
  ASSERT_TRUE(kFile.Write(BuildVectors()));
}

// RC020-VEC: the committed file is what this build makes, but for the hedged signatures.
TEST(RelayGrantVectorsTest, TheCommittedFileIsWhatTheBuildersMake) {
  EXPECT_EQ(VectorFile::Text(WithoutKeys(kFile.Read(), kSignatureKeys)),
            VectorFile::Text(WithoutKeys(BuildVectors(), kSignatureKeys)))
      << kFile.RegenerateHint();
}

// RC020-VEC: each committed signed bytes case is what the builder makes of its fields (the relay rebuilds them too).
TEST(RelayGrantVectorsTest, TheSignedBytesAreTheGrantsFields) {
  const Object committed = kFile.Read();
  const Array* cases = committed.getArray("sign_bytes");
  ASSERT_NE(cases, nullptr);
  ASSERT_EQ(cases->elements.size(), 2u);
  for (const Value& item : cases->elements) {
    const Object& row = *pp::common::asObject(item);
    Object fields = *row.getObject("grant");
    fields.set("signature", Base64Encode(ByteVector(3310, 1))); // any well-formed typed signature: not checked here
    const RelayGrant grant = RelayGrantFromJson(fields).value();
    EXPECT_EQ(BytesToHex(BuildRelayGrantSignBytes(grant)), row.getString("sign_bytes_hex").value_or(""))
        << row.getString("name").value_or("");
    EXPECT_EQ(SignedBytesVersion(BuildRelayGrantSignBytes(grant)), kRelayGrantVersion);
  }
}

// RC020-ORDER: every committed fixture gets its verdict from the receiver's order, under the committed key.
TEST(RelayGrantVectorsTest, EachFixtureGetsItsVerdict) {
  const Object committed = kFile.Read();
  const std::string key = committed.getString("recipient_public_key_b64").value_or("");
  const Array* fixtures = committed.getArray("fixtures");
  ASSERT_NE(fixtures, nullptr);
  for (const Value& item : fixtures->elements) {
    const Object& row = *pp::common::asObject(item);
    const std::string name = row.getString("name").value_or("");
    RelayGrant grant;
    const SignedReceipt receipt = ReadSignedRelayGrant(*row.getObject("grant"), key, &grant);
    EXPECT_EQ(SignedVerdictName(receipt.verdict), row.getString("verdict").value_or(""))
        << name << ": " << receipt.detail;
    EXPECT_EQ(receipt.needs_update(), AppError::IsNewerFormat(receipt.ToError())) << name;
    if (receipt.accepted()) {
      // A grant kept or forwarded writes back the JSON it came as, its extension entries included.
      EXPECT_EQ(DumpJson(RelayGrantToJson(grant)), DumpJson(*row.getObject("grant"))) << name;
    }
  }
}

} // namespace
} // namespace pbr
