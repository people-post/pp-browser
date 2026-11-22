// KEM binding v2 (docs/contracts/ACCOUNT_CARD.md § KEM binding; release-compatibility SIGNED_FORMATS § 2.3, § 6): its
// signed bytes and blob, the producer, the receiver's order (version → parse → algorithm ids → verify → extensions,
// then who signed), and the frozen vectors. The committed file is data/kem_binding_vectors.json, which pp-relay-apis
// (R2) reads byte for byte: register/finish's `kem_binding_b64`, checked and stored as received.
// PP_WRITE_KEM_BINDING_VECTORS=<path> writes a fresh one (keys from fixed seeds; ML-DSA signatures are randomized, so
// only the bindings differ between runs).

#include "foundation/crypto/KemKeyBinding.h"

#include "common/tests/VectorFile.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/crypto/TypedBlob.h"
#include "foundation/error/AppError.h"

#include "common/Serialize.hpp"

#include <gtest/gtest.h>

#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace pbr {
namespace {

using pp::ExtArea;
using pp::kMlDsa65SeedBytes;
using pp::common::Array;
using pp::common::Object;
using pp::common::Value;
using test::VectorFile;
using test::WithoutKeys;

const VectorFile kFile{PP_KEM_BINDING_VECTORS_PATH, "PP_WRITE_KEM_BINDING_VECTORS"};
/** The fields that hold a (randomized) signature. */
const std::set<std::string> kSignedKeys{"kem_binding_b64"};
constexpr uint8_t kUnknownAlgId = 0x7f;

/** An account: its ML-DSA key from a seed, its KEM key from coins, both typed as a key card carries them. */
struct Account {
  MlDsaKeyPair signing;
  std::string id;
  std::string signing_b64; // typed
  std::string kem_b64;     // typed

  static Account FromSeed(const uint8_t fill) {
    Account a;
    a.signing = MlDsa::GenerateKeyPairFromSeed(ByteVector(kMlDsa65SeedBytes, fill)).value();
    a.id = AccountIdFromMlDsaPublicKey(a.signing.public_key).value();
    a.signing_b64 = WriteTypedBlobB64(pp::BlobKind::kSignaturePublicKey, a.signing.public_key).value();
    const ByteVector kem = HybridKem::GenerateKeyPairFromSeed(ByteVector(64, static_cast<uint8_t>(fill + 1))) // coins
                               .value()
                               .public_key;
    a.kem_b64 = WriteTypedBlobB64(pp::BlobKind::kKemPublicKey, kem).value();
    return a;
  }

  KemBindingSignFn Signer() const {
    return [this](const ByteVector& bytes) -> Roe<std::string> {
      auto signature = MlDsa::Sign(signing.secret_key, bytes);
      if (!signature) {
        return signature.error();
      }
      return Base64Encode(*signature);
    };
  }

  /**
   * This account's signature over the binding bytes of `signing_key` and `kem` (typed, base64; any algorithm id, as a
   * newer app's), as a blob.
   */
  std::string Bind(const std::string& signing_key, const std::string& kem, const ExtArea& ext = {}) const {
    const ByteVector bytes = KemBindingSignBytes(Base64Decode(signing_key).value(), Base64Decode(kem).value(), ext);
    return Base64Encode(KemBindingBlob(SignTyped(signing.secret_key, bytes).value(), ext));
  }
  std::string Bind(const ExtArea& ext = {}) const { return Bind(signing_b64, kem_b64, ext); }
};

const Account& Alice() {
  static const Account a = Account::FromSeed(0x61);
  return a;
}
const Account& Bob() {
  static const Account b = Account::FromSeed(0x63);
  return b;
}

ByteVector Bytes(const std::string& b64) {
  return Base64Decode(b64).value();
}

std::string WithFirstByte(const std::string& b64, const uint8_t first) {
  ByteVector bytes = Bytes(b64);
  bytes[0] = first;
  return Base64Encode(bytes);
}

ExtArea Entry(const uint16_t type) {
  return ExtArea::FromEntries({{.type = type, .value = {0x01, 0x02, 0x03}}}).value();
}

/** A binding blob carrying `signature` and the area `ext` (not re-signed). */
std::string BlobOf(const ByteVector& signature, const ExtArea& ext = {}) {
  return Base64Encode(KemBindingBlob(signature, ext));
}

/** The typed signature a blob carries. */
ByteVector SignatureOf(const std::string& binding_b64) {
  const ByteVector blob = Bytes(binding_b64);
  SignedBlobReader in(blob);
  (void)in.U8();
  (void)in.Ext();
  return in.Bytes();
}

/** What a receiver says of a binding presented for `account_id`: the receipt's verdict, or `wrong_account`. */
std::string Judge(const std::string& account_id, const std::string& signing_key, const std::string& kem,
                  const std::string& binding) {
  const SignedReceipt receipt = ReceiveKemBinding(signing_key, kem, binding);
  if (!receipt.accepted()) {
    return std::string(SignedVerdictName(receipt.verdict));
  }
  return VerifyKemBinding(account_id, signing_key, kem, binding) ? "accepted" : "wrong_account";
}

Object Case(const std::string& name, const std::string& account_id, const std::string& signing_key,
            const std::string& kem, const std::string& binding) {
  Object row;
  row.set("name", name);
  row.set("account_id", account_id);
  row.set("signing_public_key_b64", signing_key);
  row.set("kem_public_key_b64", kem);
  row.set("kem_binding_b64", binding);
  row.set("verdict", Judge(account_id, signing_key, kem, binding));
  return row;
}

Object SignBytesCase(const std::string& name, const ExtArea& ext) {
  Object row;
  row.set("name", name);
  WriteExtB64(row, ext);
  row.set("sign_bytes_hex", BytesToHex(KemBindingSignBytes(Bytes(Alice().signing_b64), Bytes(Alice().kem_b64), ext)));
  return row;
}

std::vector<Value> ReceiveCases() {
  const Account& a = Alice();
  const Account& b = Bob();
  const std::string good = a.Bind();
  const ByteVector signature = SignatureOf(good);
  ByteVector unknown_signature = signature;
  unknown_signature[0] = kUnknownAlgId;
  ByteVector zero_signature = signature;
  zero_signature[0] = 0;
  ByteVector trailing = Bytes(good);
  trailing.push_back(0x00);
  // An area whose entries are not sorted: built by hand (ExtArea never holds one).
  ByteVector unsorted{kKemBindingVersion, 0, 0, 0, 0, 0, 0, 0, 12, 0x00, 0x02, 0, 0, 0, 0, 0x00, 0x01, 0, 0, 0, 0};
  {
    std::ostringstream tail;
    pp::OutputArchive ar(tail);
    pp::WireLenBytes wire{signature};
    ar & wire;
    const std::string packed = tail.str();
    unsorted.insert(unsorted.end(), packed.begin(), packed.end());
  }
  const std::string unknown_signing = WithFirstByte(a.signing_b64, kUnknownAlgId);
  const std::string unknown_kem = WithFirstByte(a.kem_b64, kUnknownAlgId);
  const std::string critical_signed = a.Bind(Entry(0x8101));
  std::vector<Value> rows;
  const auto add = [&rows](Object row) { rows.push_back(ObjectValue(std::move(row))); };
  add(Case("accepted", a.id, a.signing_b64, a.kem_b64, good));
  add(Case("unknown_non_critical_entry_accepted_and_kept", a.id, a.signing_b64, a.kem_b64, a.Bind(Entry(0x0101))));
  add(Case("version_3_needs_update", a.id, a.signing_b64, a.kem_b64, WithFirstByte(good, 3)));
  add(Case("version_1_older", a.id, a.signing_b64, a.kem_b64, WithFirstByte(good, 1)));
  add(Case("no_binding_older", a.id, a.signing_b64, a.kem_b64, ""));
  add(Case("bytes_after_the_signature_malformed", a.id, a.signing_b64, a.kem_b64, Base64Encode(trailing)));
  add(Case("unsorted_area_malformed", a.id, a.signing_b64, a.kem_b64, Base64Encode(unsorted)));
  add(Case("signature_algorithm_0_malformed", a.id, a.signing_b64, a.kem_b64, BlobOf(zero_signature)));
  add(Case("unknown_signature_algorithm_needs_update", a.id, a.signing_b64, a.kem_b64, BlobOf(unknown_signature)));
  add(Case("unknown_signing_key_algorithm_needs_update", a.id, unknown_signing, a.kem_b64,
           a.Bind(unknown_signing, a.kem_b64)));
  add(Case("unknown_kem_key_algorithm_needs_update", a.id, a.signing_b64, unknown_kem,
           a.Bind(a.signing_b64, unknown_kem)));
  add(Case("another_kem_key_forged", a.id, a.signing_b64, b.kem_b64, good));
  add(Case("signed_by_another_account_forged", a.id, a.signing_b64, a.kem_b64, b.Bind(a.signing_b64, a.kem_b64)));
  add(Case("unknown_critical_entry_needs_update", a.id, a.signing_b64, a.kem_b64, critical_signed));
  add(Case("unknown_critical_entry_not_signed_forged", a.id, a.signing_b64, a.kem_b64,
           BlobOf(signature, Entry(0x8101))));
  add(Case("another_accounts_binding_wrong_account", a.id, b.signing_b64, b.kem_b64, b.Bind()));
  return rows;
}

Object BuildVectors() {
  Object root;
  root.set("format", "pp-browser kem binding vectors v1");
  root.set("about",
           "KEM binding v2 (release-compatibility SIGNED_FORMATS 2.3; docs/contracts/ACCOUNT_CARD.md, KEM binding): "
           "sign_bytes are frozen (hex); each receive case gives the receiver's verdict (SIGNED_FORMATS 1.1), then "
           "wrong_account when the signing key does not hash to account_id. Signatures are randomized: compare "
           "without kem_binding_b64, and check the committed bindings give their verdicts.");
  root.set("domain", kKemBindingDomain);
  root.set("binding_version", static_cast<int64_t>(kKemBindingVersion));
  root.set("account_seed_hex", BytesToHex(ByteVector(kMlDsa65SeedBytes, 0x61)));
  root.set("kem_coins_hex", BytesToHex(ByteVector(64, 0x62)));
  root.set("account_id", Alice().id);
  root.set("signing_public_key_b64", Alice().signing_b64);
  root.set("kem_public_key_b64", Alice().kem_b64);
  std::vector<Value> sign_bytes;
  sign_bytes.push_back(ObjectValue(SignBytesCase("empty_area", {})));
  sign_bytes.push_back(ObjectValue(SignBytesCase("one_unknown_non_critical_entry", Entry(0x0101))));
  root.set("sign_bytes", ArrayValue(std::move(sign_bytes)));
  root.set("receive", ArrayValue(ReceiveCases()));
  return root;
}

TEST(KemBindingVectorsTest, WriteAFreshFileWhenAsked) {
  if (!kFile.WriteRequested()) {
    GTEST_SKIP() << kFile.SkipReason();
  }
  ASSERT_TRUE(kFile.Write(BuildVectors()));
}

// RC020 § 6: the committed file is what the builders make (but for the randomized bindings).
TEST(KemBindingVectorsTest, TheCommittedFileIsWhatTheBuildersMake) {
  EXPECT_EQ(VectorFile::Text(WithoutKeys(kFile.Read(), kSignedKeys)),
            VectorFile::Text(WithoutKeys(BuildVectors(), kSignedKeys)))
      << kFile.RegenerateHint();
}

// RC020 § 6: every committed binding gives its verdict, as the relay must find it.
TEST(KemBindingVectorsTest, EachCommittedCaseGivesItsVerdict) {
  const Object committed = kFile.Read();
  const Array* cases = committed.getArray("receive");
  ASSERT_NE(cases, nullptr);
  ASSERT_FALSE(cases->elements.empty());
  for (const Value& item : cases->elements) {
    const Object* row = asObject(item);
    ASSERT_NE(row, nullptr);
    const auto field = [row](const char* key) { return row->getString(key).value_or(""); };
    EXPECT_EQ(Judge(field("account_id"), field("signing_public_key_b64"), field("kem_public_key_b64"),
                    field("kem_binding_b64")),
              field("verdict"))
        << field("name");
  }
}

TEST(KemKeyBindingTest, SignBytesAreTheDomainVersionBothTypedKeysAndTheArea) {
  const Account& a = Alice();
  const ByteVector signing = Bytes(a.signing_b64);
  const ByteVector kem = Bytes(a.kem_b64);
  ASSERT_EQ(signing.size(), 1953u);
  ASSERT_EQ(kem.size(), 1185u);
  const ByteVector bytes = KemBindingSignBytes(signing, kem);
  const std::string domain = "pp-browser:kem-binding";
  ASSERT_EQ(bytes.size(), domain.size() + 1 + 1 + 8 + signing.size() + 8 + kem.size() + 8);
  EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + static_cast<long>(domain.size())), domain);
  EXPECT_EQ(bytes[domain.size()], 0);
  EXPECT_EQ(SignedBytesVersion(bytes), std::optional<uint8_t>(2));
  SignedBlobReader in(std::span<const uint8_t>(bytes).subspan(domain.size() + 2));
  EXPECT_EQ(in.Bytes(), signing);
  EXPECT_EQ(in.Bytes(), kem);
  EXPECT_TRUE(in.Ext().empty());
  EXPECT_TRUE(in.Done());
}

TEST(KemKeyBindingTest, TheBlobIsTheVersionTheAreaAndTheTypedSignature) {
  const std::string binding = Alice().Bind(Entry(0x0101));
  const ByteVector blob = Bytes(binding);
  SignedBlobReader in(blob);
  EXPECT_EQ(in.U8(), 2);
  EXPECT_EQ(in.Ext(), Entry(0x0101)) << "the area is kept in the blob";
  const ByteVector signature = in.Bytes();
  EXPECT_TRUE(in.Done());
  EXPECT_EQ(signature.size(), 3310u);
  EXPECT_EQ(signature[0], pp::kSignatureAlgMlDsa65);
}

TEST(KemKeyBindingTest, TheAccountsOwnBindingVerifies) {
  const Account& a = Alice();
  EXPECT_TRUE(VerifyKemBinding(a.id, a.signing_b64, a.kem_b64, a.Bind()));
  EXPECT_TRUE(VerifyKemBinding(a.id, a.signing_b64, a.kem_b64, a.Bind(Entry(0x0101))))
      << "an unknown non-critical entry is ignored";
}

TEST(KemKeyBindingTest, AKeyTheAccountDidNotBindIsRefusedAndNotAsUpdateNeeded) {
  const Account& a = Alice();
  const Account& b = Bob();
  for (const auto& [kem, binding, signing, account] :
       {std::tuple{b.kem_b64, a.Bind(), a.signing_b64, a.id}, // another KEM key under the binding
        std::tuple{b.kem_b64, b.Bind(), b.signing_b64, a.id}, // someone else's keys for this account
        std::tuple{b.kem_b64, b.Bind(a.signing_b64, b.kem_b64), a.signing_b64, a.id},        // signed by someone else
        std::tuple{a.kem_b64, Base64Encode(ByteVector{2, 1, 2, 3}), a.signing_b64, a.id}}) { // v2, not parsing
    auto verified = VerifyKemBinding(account, signing, kem, binding);
    ASSERT_FALSE(verified);
    EXPECT_FALSE(AppError::IsNewerFormat(verified.error())) << verified.error().message;
  }
}

// RC020-ORDER: a newer binding, an unknown algorithm id and (once verified) an unknown critical entry are update
// needed; an unsigned critical entry is a forgery, not "update needed".
TEST(KemKeyBindingTest, TheReceiversOrder) {
  const Account& a = Alice();
  const std::string good = a.Bind();
  EXPECT_EQ(ReceiveKemBinding(a.signing_b64, a.kem_b64, WithFirstByte(good, 3)).verdict, SignedVerdict::NeedsUpdate);
  EXPECT_EQ(ReceiveKemBinding(a.signing_b64, a.kem_b64, Base64Encode(ByteVector{3})).verdict,
            SignedVerdict::NeedsUpdate)
      << "nothing after a newer version is read";
  EXPECT_EQ(ReceiveKemBinding(a.signing_b64, a.kem_b64, WithFirstByte(good, 1)).verdict, SignedVerdict::Older);
  EXPECT_EQ(ReceiveKemBinding(a.signing_b64, a.kem_b64, "").verdict, SignedVerdict::Older);
  EXPECT_EQ(ReceiveKemBinding(a.signing_b64, a.kem_b64, "%%%").verdict, SignedVerdict::Malformed);
  EXPECT_EQ(ReceiveKemBinding(a.signing_b64, a.kem_b64, Base64Encode(ByteVector{2, 0, 0})).verdict,
            SignedVerdict::Malformed);
  ByteVector unknown = SignatureOf(good);
  unknown[0] = kUnknownAlgId;
  EXPECT_EQ(ReceiveKemBinding(a.signing_b64, a.kem_b64, BlobOf(unknown)).verdict, SignedVerdict::NeedsUpdate);
  const SignedReceipt critical = ReceiveKemBinding(a.signing_b64, a.kem_b64, a.Bind(Entry(0x8101)));
  EXPECT_EQ(critical.verdict, SignedVerdict::NeedsUpdate);
  EXPECT_TRUE(AppError::IsNewerFormat(VerifyKemBinding(a.id, a.signing_b64, a.kem_b64, a.Bind(Entry(0x8101))).error()));
  EXPECT_EQ(ReceiveKemBinding(a.signing_b64, a.kem_b64, BlobOf(SignatureOf(good), Entry(0x8101))).verdict,
            SignedVerdict::Forged);
}

TEST(KemKeyBindingTest, TheProducerSignsTheTypedKeysAndRefusesKeysItCannotRead) {
  const Account& a = Alice();
  const std::string made = SignKemBindingB64(a.signing_b64, a.kem_b64, a.Signer()).value();
  EXPECT_TRUE(VerifyKemBinding(a.id, a.signing_b64, a.kem_b64, made));
  EXPECT_FALSE(SignKemBindingB64(Base64Encode(a.signing.public_key), a.kem_b64, a.Signer()))
      << "a raw (untyped) signing key";
  auto unknown = SignKemBindingB64(WithFirstByte(a.signing_b64, kUnknownAlgId), a.kem_b64, a.Signer());
  ASSERT_FALSE(unknown);
  EXPECT_TRUE(AppError::IsNewerFormat(unknown.error()));
}

} // namespace
} // namespace pbr
