// The relay operator set and certificate (release-compatibility SIGNED_FORMATS § 3.1-3.2, § 6): parse, verify against
// the pinned relay id, keep the highest; and relay_certificate_vectors.json, which pp-relay-apis (R3: the registry's
// load check, `scripts/relay-operator.mjs`) and pp-node read byte for byte.
//
// ML-DSA signing is hedged, so the certificates differ between runs: the file's certificates (`certificate_b64`,
// `change_b64`) are frozen as written and must read as each case says; everything else (keys from seeds, the set's
// encoding, the relay id, the signed bytes' hashes, the verdicts) is what the builders below make, exactly.

#include "lib/net/RelayCertificate.h"

#include "common/tests/VectorFile.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/Sha256Hex.h"
#include "foundation/crypto/TypedBlob.h"
#include "foundation/error/AppError.h"

#include "crypto/PeerId.h"

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace pbr {
namespace {

using pp::common::Array;
using pp::common::Object;
using pp::common::Value;
using test::VectorFile;
using test::WithoutKeys;

const VectorFile kVectors{PP_RELAY_CERTIFICATE_VECTORS_PATH, "PP_WRITE_RELAY_CERTIFICATE_VECTORS"};
/** What a regeneration changes: the hedged signatures and what hashes them. */
const std::set<std::string> kSigned = {"certificate_b64", "change_b64", "certificate_signed_bytes_sha256"};

constexpr int64_t kIssuedAt = 1793404800000;  // 2026-10-31T00:00:00Z
constexpr int64_t kNowMs = 1795046400000;     // 2026-11-19T00:00:00Z: the time every case is read at
constexpr int64_t kExpiredAt = 1794960000000; // 2026-11-18T00:00:00Z: before kNowMs
constexpr int64_t kDayMs = 24 * 60 * 60 * 1000;
const std::vector<std::string> kOrigins = {"https://relay.example/api/relay", "https://relay-b.example/api/relay"};

std::string Hex(const ByteVector& bytes) {
  return BytesToHex(bytes);
}

std::string Sha256Of(const ByteVector& bytes) {
  return Sha256Hex(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

/** A key from a 32-byte seed of one repeated byte, as the vector file names it. */
struct SeededKey {
  std::string name;
  uint8_t seed = 0;
  MlDsaKeyPair pair;
  ByteVector typed;
};

SeededKey Seeded(const std::string& name, const uint8_t seed) {
  SeededKey key{.name = name, .seed = seed};
  key.pair = MlDsa::GenerateKeyPairFromSeed(ByteVector(pp::kMlDsa65SeedBytes, seed)).value();
  key.typed = TypedSigningPublicKey(key.pair.public_key).value();
  return key;
}

/** The keys of the file: operators 1-3 (2 of 3), operator 4 (a set change), an outsider, the relay's keys. */
struct Keys {
  SeededKey op1 = Seeded("operator_1", 0x01);
  SeededKey op2 = Seeded("operator_2", 0x02);
  SeededKey op3 = Seeded("operator_3", 0x03);
  SeededKey op4 = Seeded("operator_4", 0x04);
  SeededKey outsider = Seeded("outsider", 0x09);
  SeededKey transport1 = Seeded("transport_1", 0x11);
  SeededKey transport2 = Seeded("transport_2", 0x12);
  SeededKey signing = Seeded("signing", 0x21);
  SeededKey other_operator = Seeded("other_relay_operator", 0x31);

  std::vector<const SeededKey*> All() const {
    return {&op1, &op2, &op3, &op4, &outsider, &transport1, &transport2, &signing, &other_operator};
  }
};

const Keys& TheKeys() {
  static const Keys keys;
  return keys;
}

OperatorSet Founding() {
  const Keys& k = TheKeys();
  return MakeOperatorSet(2, {k.op1.typed, k.op2.typed, k.op3.typed}).value();
}

OperatorSet Replaced() {
  const Keys& k = TheKeys();
  return MakeOperatorSet(2, {k.op1.typed, k.op2.typed, k.op4.typed}).value();
}

/** `key` signing as a member of `set` (its index there). */
OperatorSigner As(const SeededKey& key, const OperatorSet& set) {
  return OperatorSigner{.signer_index = static_cast<uint8_t>(set.IndexOf(key.typed).value()),
                        .secret_key = key.pair.secret_key};
}

RelayCertificateBody Body(const uint64_t sequence, const SeededKey& transport) {
  return RelayCertificateBody{.founding = Founding(),
                              .changes = {},
                              .sequence = sequence,
                              .issued_at = kIssuedAt + static_cast<int64_t>(sequence - 1) * kDayMs,
                              .expires_at = 0,
                              .origins = kOrigins,
                              .transport_key = transport.typed,
                              .signing_key = TheKeys().signing.typed,
                              .ext = {}};
}

ByteVector SignedBy(const RelayCertificateBody& body, const std::vector<OperatorSigner>& signers) {
  return SignRelayCertificate(body, signers).value();
}

ByteVector TwoOfThree(const RelayCertificateBody& body) {
  const OperatorSet set = Founding();
  return SignedBy(body, {As(TheKeys().op1, set), As(TheKeys().op3, set)});
}

pp::ExtArea Ext(const uint16_t type, const ByteVector& value) {
  return pp::ExtArea::FromEntries({pp::ExtArea::Entry{.type = type, .value = value}}).value();
}

/** One case: a certificate, what it is read against, and what reading it must say. */
struct Case {
  std::string name;
  std::string description;
  ByteVector certificate;
  /** The relay id it is checked against. */
  RelayId pinned;
  /** The origin it was asked at (it must list it). */
  std::string asked_origin = kOrigins[0];
  /** The case whose certificate is held when this one is offered ("": none). */
  std::string held;
  /** ReceiveRelayCertificate's verdict. */
  std::string verdict;
  /** What the client does: adopted, same, lower, conflict, origin_not_listed; refused when not accepted. */
  std::string outcome;
  /** The body its signed bytes are made of (none: the bytes are not a certificate's). */
  std::optional<RelayCertificateBody> body;
  /** The case's set change, kept to freeze its signed bytes. */
  std::optional<OperatorSetChange> change;
};

std::vector<Case> BuildCases() {
  const Keys& k = TheKeys();
  const OperatorSet founding = Founding();
  const RelayId ours = founding.Id();
  std::vector<Case> cases;
  const auto add = [&cases, &ours](Case c) {
    if (c.pinned.empty()) {
      c.pinned = ours;
    }
    cases.push_back(std::move(c));
  };

  const RelayCertificateBody one = Body(1, k.transport1);
  add({.name = "certificate_1",
       .description = "sequence 1 for transport key 1, signed by operators 1 and 3 of the 2-of-3 set: kept",
       .certificate = TwoOfThree(one),
       .verdict = "accepted",
       .outcome = "adopted",
       .body = one});
  const RelayCertificateBody two = Body(2, k.transport2);
  add({.name = "certificate_2",
       .description = "sequence 2 for a new transport key (a host move): kept over certificate 1",
       .certificate = SignedBy(two, {As(k.op2, founding), As(k.op3, founding)}),
       .held = "certificate_1",
       .verdict = "accepted",
       .outcome = "adopted",
       .body = two});
  add({.name = "certificate_2_again",
       .description = "certificate 2 offered while it is held, byte for byte: nothing changes",
       .certificate = cases[1].certificate,
       .held = "certificate_2",
       .verdict = "accepted",
       .outcome = "same",
       .body = two});

  OperatorSetChange change{.set_generation = 2, .new_set = Replaced(), .ext = {}, .signatures = {}};
  change = SignOperatorSetChange(ours, change, {As(k.op1, founding), As(k.op2, founding)}).value();
  RelayCertificateBody changed = Body(3, k.transport2);
  changed.changes = {change};
  add({.name = "with_set_change",
       .description =
           "operator 3 replaced by operator 4 (set generation 2, signed by operators 1 and 2 of the founding "
           "set); sequence 3 signed by operators 2 and 4 of the new set: kept, the relay id unchanged",
       .certificate = SignedBy(changed, {As(k.op2, Replaced()), As(k.op4, Replaced())}),
       .held = "certificate_2",
       .verdict = "accepted",
       .outcome = "adopted",
       .body = changed,
       .change = change});

  RelayCertificateBody noted = Body(4, k.transport2);
  noted.ext = Ext(0x0001, {0xAB, 0xCD});
  add({.name = "unknown_non_critical_entry",
       .description = "sequence 4 with an unknown non-critical entry (type 0x0001): verifies, kept with its bytes",
       .certificate = TwoOfThree(noted),
       .held = "certificate_2",
       .verdict = "accepted",
       .outcome = "adopted",
       .body = noted});

  // Refused.
  add({.name = "one_signature",
       .description = "only operator 1 signed: the 2-of-3 set needs two",
       .certificate = SignedBy(one, {As(k.op1, founding)}),
       .verdict = "forged",
       .outcome = "refused",
       .body = one});
  {
    const RelayCertificateBody& body = one;
    const ByteVector message = RelayCertificateSignedBytes(body);
    const ByteVector signature = SignTyped(k.op1.pair.secret_key, message).value();
    const uint8_t index = static_cast<uint8_t>(founding.IndexOf(k.op1.typed).value());
    add({.name = "index_twice",
         .description = "operator 1's signature twice under its index: two signatures, one operator",
         .certificate = EncodeRelayCertificate(
             body, {{.signer_index = index, .signature = signature}, {.signer_index = index, .signature = signature}}),
         .verdict = "malformed",
         .outcome = "refused",
         .body = body});
    add({.name = "signer_index_outside_set",
         .description = "a second signature under index 3 of a 3-key set",
         .certificate = EncodeRelayCertificate(
             body, {{.signer_index = index, .signature = signature}, {.signer_index = 3, .signature = signature}}),
         .verdict = "malformed",
         .outcome = "refused",
         .body = body});
  }
  {
    // The outsider signs under operator 3's index: a key outside the set.
    OperatorSigner outsider{.signer_index = static_cast<uint8_t>(founding.IndexOf(k.op3.typed).value()),
                            .secret_key = k.outsider.pair.secret_key};
    add({.name = "key_outside_set",
         .description = "operator 1 and a key outside the set (under operator 3's index)",
         .certificate = SignedBy(one, {As(k.op1, founding), outsider}),
         .verdict = "forged",
         .outcome = "refused",
         .body = one});
  }
  {
    const OperatorSet other = MakeOperatorSet(1, {k.other_operator.typed}).value();
    RelayCertificateBody body = Body(1, k.transport1);
    body.founding = other;
    add({.name = "other_relay_id",
         .description = "a valid certificate of another relay (its 1-of-1 set), served for ours",
         .certificate = SignedBy(body, {As(k.other_operator, other)}),
         .verdict = "other_relay",
         .outcome = "refused",
         .body = body});
  }
  add({.name = "lower_sequence",
       .description = "certificate 1 offered while certificate 2 is held: never below",
       .certificate = cases[0].certificate,
       .held = "certificate_2",
       .verdict = "accepted",
       .outcome = "lower",
       .body = one});
  {
    RelayCertificateBody body = two;
    body.issued_at += 1;
    add({.name = "same_sequence_other_bytes",
         .description = "another sequence-2 certificate (issued a millisecond later) while certificate 2 is held: "
                        "refused, logged as an error, the first stays",
         .certificate = TwoOfThree(body),
         .held = "certificate_2",
         .verdict = "accepted",
         .outcome = "conflict",
         .body = body});
  }
  add({.name = "origin_not_listed",
       .description = "certificate 1 served at an origin it does not list (a relay fronting another)",
       .certificate = cases[0].certificate,
       .asked_origin = "https://elsewhere.example/api/relay",
       .verdict = "accepted",
       .outcome = "origin_not_listed",
       .body = one});
  {
    RelayCertificateBody body = Body(5, k.transport2);
    body.expires_at = kExpiredAt;
    add({.name = "expires_at_past",
         .description = "sequence 5 that expired before the time it is read at",
         .certificate = TwoOfThree(body),
         .verdict = "expired",
         .outcome = "refused",
         .body = body});
  }
  {
    RelayCertificateBody body = Body(5, k.transport2);
    body.ext = Ext(0x8001, {0x01});
    add({.name = "unknown_critical_entry",
         .description = "sequence 5 with an unknown critical entry (type 0x8001): verifies, then update needed",
         .certificate = TwoOfThree(body),
         .verdict = "needs_update",
         .outcome = "refused",
         .body = body});
  }
  {
    ByteVector blob = cases[0].certificate;
    blob[0] = 2;
    add({.name = "version_2",
         .description = "certificate 1 with version byte 2: update needed, nothing verified",
         .certificate = blob,
         .verdict = "needs_update",
         .outcome = "refused"});
    blob[0] = 0;
    add({.name = "version_0",
         .description = "certificate 1 with version byte 0: made by an older tool",
         .certificate = blob,
         .verdict = "older",
         .outcome = "refused"});
  }
  {
    ByteVector blob = cases[0].certificate;
    blob.resize(blob.size() - 1);
    add({.name = "truncated",
         .description = "certificate 1 without its last byte",
         .certificate = blob,
         .verdict = "malformed",
         .outcome = "refused"});
  }
  {
    OperatorSetChange skipped{.set_generation = 3, .new_set = Replaced(), .ext = {}, .signatures = {}};
    skipped = SignOperatorSetChange(ours, skipped, {As(k.op1, founding), As(k.op2, founding)}).value();
    RelayCertificateBody body = Body(3, k.transport2);
    body.changes = {skipped};
    add({.name = "set_change_skips_a_generation",
         .description = "a set change of generation 3 after the founding set (generation 1)",
         .certificate = SignedBy(body, {As(k.op2, Replaced()), As(k.op4, Replaced())}),
         .verdict = "forged",
         .outcome = "refused",
         .body = body,
         .change = skipped});
  }
  {
    RelayCertificateBody body = Body(5, k.transport2);
    body.transport_key = k.op2.typed;
    add({.name = "transport_key_is_an_operator_key",
         .description = "sequence 5 whose transport key is operator 2's",
         .certificate = TwoOfThree(body),
         .verdict = "invalid",
         .outcome = "refused",
         .body = body});
  }
  return cases;
}

/** What a client does with `certificate_b64` asked at `asked_origin`, holding `held_b64` ("": nothing). */
std::pair<std::string, std::string> Read(const std::string& certificate_b64, const RelayId& pinned,
                                         const std::string& asked_origin, const std::string& held_b64) {
  const RelayCertificateCheck check = ReceiveRelayCertificateB64(certificate_b64, pinned, kNowMs);
  const std::string verdict(RelayCertificateVerdictName(check.verdict));
  if (!check.accepted()) {
    return {verdict, "refused"};
  }
  if (!check.certificate->Lists(asked_origin)) {
    return {verdict, "origin_not_listed"};
  }
  std::optional<RelayCertificate> held;
  if (!held_b64.empty()) {
    RelayCertificateCheck held_check = ReceiveRelayCertificateB64(held_b64, pinned, kNowMs);
    EXPECT_TRUE(held_check.accepted()) << held_check.detail;
    held = std::move(held_check.certificate);
  }
  return {
      verdict,
      std::string(RelayCertificateOfferName(CompareRelayCertificates(held ? &*held : nullptr, *check.certificate)))};
}

Object KeyObject(const SeededKey& key) {
  Object out;
  out.set("name", key.name);
  out.set("seed_hex", Hex(ByteVector(pp::kMlDsa65SeedBytes, key.seed)));
  out.set("public_key_b64", Base64Encode(key.typed));
  out.set("public_key_sha256", Sha256Of(key.typed));
  out.set("peer_id", pp::PeerIdFromMlDsaPublicKey(key.pair.public_key).value());
  return out;
}

Object CaseObject(const Case& c) {
  Object out;
  out.set("name", c.name);
  out.set("description", c.description);
  out.set("pinned_relay_id", c.pinned.ToString());
  out.set("asked_origin", c.asked_origin);
  if (!c.held.empty()) {
    out.set("held", c.held);
  }
  out.set("certificate_b64", Base64Encode(c.certificate));
  if (c.body) {
    const ByteVector signed_bytes = RelayCertificateSignedBytes(*c.body);
    // With a set change the body holds the change's (hedged) signatures: its hash changes with them.
    out.set(c.change ? "certificate_signed_bytes_sha256" : "signed_bytes_sha256", Sha256Of(signed_bytes));
    out.set("sequence", static_cast<int64_t>(c.body->sequence));
    out.set("issued_at", c.body->issued_at);
    out.set("expires_at", c.body->expires_at);
    std::vector<Value> origins;
    for (const std::string& origin : c.body->origins) {
      origins.emplace_back(origin);
    }
    out.set("origins", pp::common::makeArray(std::move(origins)));
    const TypedRead transport = ReadTypedBlob(pp::BlobKind::kSignaturePublicKey, c.body->transport_key, "transport");
    out.set("audience_peer_id", pp::PeerIdFromMlDsaPublicKey(transport.raw).value());
  }
  if (c.change) {
    out.set("change_b64", Base64Encode(EncodeOperatorSetChange(*c.change)));
    out.set("change_signed_bytes_sha256", Sha256Of(OperatorSetChangeSignedBytes(c.pinned, *c.change)));
    out.set("change_set_generation", static_cast<int64_t>(c.change->set_generation));
  }
  out.set("verdict", c.verdict);
  out.set("outcome", c.outcome);
  return out;
}

Object BuildVectors(const std::vector<Case>& cases) {
  const OperatorSet founding = Founding();
  const ByteVector encoding = founding.Encode();
  Object root;
  root.set("description",
           "Relay operator set and certificate (pp-browser projects/release-compatibility/SIGNED_FORMATS.md § 3, § 6). "
           "Keys are ML-DSA-65 from a 32-byte seed of one repeated byte, typed (alg id 1 first). Every case is read "
           "at now_ms against pinned_relay_id, asked at asked_origin, holding the certificate of the case `held` "
           "names: `verdict` is the certificate's (accepted, older, needs_update, malformed, forged, other_relay, "
           "expired, invalid), `outcome` what the client does (adopted, same, lower, conflict, origin_not_listed; "
           "refused). Signatures are hedged: certificate_b64 and change_b64 are frozen as written, every other value "
           "is rebuilt exactly.");
  root.set("version", int64_t{1});
  root.set("now_ms", kNowMs);
  std::vector<Value> keys;
  for (const SeededKey* key : TheKeys().All()) {
    keys.emplace_back(pp::common::ObjectValue(KeyObject(*key)));
  }
  root.set("keys", pp::common::makeArray(std::move(keys)));

  Object set;
  set.set("threshold", static_cast<int64_t>(founding.threshold));
  std::vector<Value> order;
  for (const ByteVector& typed : founding.keys) {
    for (const SeededKey* key : TheKeys().All()) {
      if (key->typed == typed) {
        order.emplace_back(key->name);
      }
    }
  }
  set.set("keys_in_order", pp::common::makeArray(std::move(order)));
  set.set("encoding_bytes", static_cast<int64_t>(encoding.size()));
  set.set("encoding_sha256", Sha256Of(encoding));
  set.set("relay_id_hash_input_sha256", Sha256Of(RelayIdHashInput(encoding)));
  set.set("relay_id", founding.Id().ToString());
  root.set("operator_set", pp::common::ObjectValue(std::move(set)));

  Object replaced;
  replaced.set("threshold", static_cast<int64_t>(Replaced().threshold));
  replaced.set("encoding_sha256", Sha256Of(Replaced().Encode()));
  replaced.set("relay_id_if_founding", Replaced().Id().ToString());
  root.set("replaced_operator_set", pp::common::ObjectValue(std::move(replaced)));

  // The first certificate's signed bytes in full, so a writer can find where it differs.
  root.set("certificate_1_signed_bytes_hex", Hex(RelayCertificateSignedBytes(*cases[0].body)));

  std::vector<Value> rows;
  for (const Case& c : cases) {
    rows.emplace_back(pp::common::ObjectValue(CaseObject(c)));
  }
  root.set("cases", pp::common::makeArray(std::move(rows)));
  return root;
}

const std::vector<Case>& Fresh() {
  static const std::vector<Case> cases = BuildCases();
  return cases;
}

std::map<std::string, const Object*> ByName(const Array& cases) {
  std::map<std::string, const Object*> out;
  for (const Value& item : cases.elements) {
    if (const Object* row = pp::common::asObject(item)) {
      out[row->getString("name").value_or("")] = row;
    }
  }
  return out;
}

TEST(RelayCertificateVectorsTest, WriteAFreshFileWhenAsked) {
  if (!kVectors.WriteRequested()) {
    GTEST_SKIP() << kVectors.SkipReason();
  }
  ASSERT_TRUE(kVectors.Write(BuildVectors(Fresh())));
}

// RC020-CERT: the builders make what the file holds, signatures aside.
TEST(RelayCertificateVectorsTest, TheCommittedFileIsWhatTheBuildersMake) {
  EXPECT_EQ(VectorFile::Text(WithoutKeys(kVectors.Read(), kSigned)),
            VectorFile::Text(WithoutKeys(BuildVectors(Fresh()), kSigned)))
      << kVectors.RegenerateHint();
}

// RC020-CERT: every committed certificate reads as its case says (they are frozen as written: the relay reads these).
TEST(RelayCertificateVectorsTest, EveryCommittedCertificateReadsAsItsCaseSays) {
  const Object file = kVectors.Read();
  const Array* cases = file.getArray("cases");
  ASSERT_NE(cases, nullptr);
  ASSERT_EQ(cases->elements.size(), Fresh().size());
  const auto by_name = ByName(*cases);
  for (const auto& [name, row] : by_name) {
    const RelayId pinned = RelayId::Parse(row->getString("pinned_relay_id").value_or("")).value_or(RelayId{});
    std::string held_b64;
    if (const auto held = row->getString("held")) {
      ASSERT_TRUE(by_name.contains(*held)) << name;
      held_b64 = by_name.at(*held)->getString("certificate_b64").value_or("");
    }
    const auto [verdict, outcome] = Read(row->getString("certificate_b64").value_or(""), pinned,
                                         row->getString("asked_origin").value_or(""), held_b64);
    EXPECT_EQ(verdict, row->getString("verdict").value_or("")) << name;
    EXPECT_EQ(outcome, row->getString("outcome").value_or("")) << name;
    // An accepted certificate's signed bytes are rebuilt from what it carries: the frozen hash.
    const RelayCertificateCheck check =
        ReceiveRelayCertificateB64(row->getString("certificate_b64").value_or(""), pinned, kNowMs);
    if (check.accepted()) {
      const std::string key =
          row->contains("signed_bytes_sha256") ? "signed_bytes_sha256" : "certificate_signed_bytes_sha256";
      EXPECT_EQ(Sha256Of(RelayCertificateSignedBytes(check.certificate->body)), row->getString(key).value_or(""))
          << name;
      EXPECT_EQ(Base64Encode(check.certificate->blob), row->getString("certificate_b64").value_or("")) << name;
      EXPECT_EQ(EncodeRelayCertificate(check.certificate->body, check.certificate->signatures), check.certificate->blob)
          << name << ": the blob is its fields' one encoding";
      EXPECT_EQ(check.certificate->audience_peer_id, row->getString("audience_peer_id").value_or("")) << name;
      if (const auto change_b64 = row->getString("change_b64")) {
        ASSERT_EQ(check.certificate->body.changes.size(), 1u) << name;
        const OperatorSetChange& change = check.certificate->body.changes[0];
        EXPECT_EQ(Base64Encode(EncodeOperatorSetChange(change)), *change_b64) << name;
        EXPECT_EQ(Sha256Of(OperatorSetChangeSignedBytes(pinned, change)),
                  row->getString("change_signed_bytes_sha256").value_or(""))
            << name;
      }
    }
  }
}

// RC020-CERT: freshly signed certificates read as their cases say too (the builders' expectations are hand-written).
TEST(RelayCertificateVectorsTest, FreshCertificatesReadAsTheirCasesSay) {
  std::map<std::string, const Case*> by_name;
  for (const Case& c : Fresh()) {
    by_name[c.name] = &c;
  }
  for (const Case& c : Fresh()) {
    const std::string held = c.held.empty() ? std::string() : Base64Encode(by_name.at(c.held)->certificate);
    const auto [verdict, outcome] = Read(Base64Encode(c.certificate), c.pinned, c.asked_origin, held);
    EXPECT_EQ(verdict, c.verdict) << c.name;
    EXPECT_EQ(outcome, c.outcome) << c.name;
  }
}

// The vectors' set is what the operators' tool writes: sorted by key bytes, threshold first.
TEST(RelayCertificateTest, AnOperatorSetIsSortedAndItsHashIsTheRelayId) {
  const Keys& k = TheKeys();
  const OperatorSet a = MakeOperatorSet(2, {k.op3.typed, k.op1.typed, k.op2.typed}).value();
  const OperatorSet b = MakeOperatorSet(2, {k.op1.typed, k.op2.typed, k.op3.typed}).value();
  EXPECT_EQ(a, b);
  EXPECT_TRUE(std::is_sorted(a.keys.begin(), a.keys.end()));
  const ByteVector encoding = a.Encode();
  ASSERT_GE(encoding.size(), 2u);
  EXPECT_EQ(encoding[0], 2);
  EXPECT_EQ(encoding[1], 3);
  EXPECT_EQ(encoding.size(), 2 + 3 * (8 + 1953));
  EXPECT_EQ(a.Id(), RelayId::OfOperatorSet(encoding));
  EXPECT_NE(a.Id(), MakeOperatorSet(1, {k.op1.typed, k.op2.typed, k.op3.typed}).value().Id())
      << "the threshold is part of the set";
}

TEST(RelayCertificateTest, AnOperatorSetRefusesBadShapes) {
  const Keys& k = TheKeys();
  EXPECT_FALSE(MakeOperatorSet(1, {}));
  EXPECT_FALSE(MakeOperatorSet(0, {k.op1.typed}));
  EXPECT_FALSE(MakeOperatorSet(2, {k.op1.typed}));
  EXPECT_FALSE(MakeOperatorSet(1, {k.op1.typed, k.op1.typed}));
  EXPECT_FALSE(MakeOperatorSet(1, {k.op1.pair.public_key})) << "an untyped key";
  std::vector<ByteVector> nine;
  for (uint8_t seed = 0x40; seed < 0x49; ++seed) {
    nine.push_back(Seeded("k", seed).typed);
  }
  EXPECT_FALSE(MakeOperatorSet(1, nine));
  nine.pop_back();
  EXPECT_TRUE(MakeOperatorSet(8, nine)) << "8 of 8 is the largest";
}

TEST(RelayCertificateTest, AFirstUseTakesTheCertificatesOwnRelayId) {
  const RelayCertificateCheck check = ReceiveRelayCertificate(Fresh()[0].certificate, RelayId{}, kNowMs);
  ASSERT_TRUE(check.accepted()) << check.detail;
  EXPECT_EQ(check.certificate->relay_id, Founding().Id());
  EXPECT_EQ(check.certificate->set_in_force, Founding());
  EXPECT_EQ(check.certificate->body.sequence, 1u);
}

TEST(RelayCertificateTest, TheSetInForceIsTheLastChangesSet) {
  const Case& changed = Fresh()[3];
  ASSERT_EQ(changed.name, "with_set_change");
  const RelayCertificateCheck check = ReceiveRelayCertificate(changed.certificate, Founding().Id(), kNowMs);
  ASSERT_TRUE(check.accepted()) << check.detail;
  EXPECT_EQ(check.certificate->relay_id, Founding().Id()) << "a set change keeps the relay id";
  EXPECT_EQ(check.certificate->set_in_force, Replaced());
}

TEST(RelayCertificateTest, AnUnknownNonCriticalEntryIsKeptWithItsBytes) {
  const Case& noted = Fresh()[4];
  ASSERT_EQ(noted.name, "unknown_non_critical_entry");
  const RelayCertificateCheck check = ReceiveRelayCertificate(noted.certificate, Founding().Id(), kNowMs);
  ASSERT_TRUE(check.accepted()) << check.detail;
  ASSERT_EQ(check.certificate->body.ext.entries().size(), 1u);
  EXPECT_EQ(check.certificate->body.ext.entries()[0].type, 0x0001);
  EXPECT_EQ(check.certificate->blob, noted.certificate);
}

TEST(RelayCertificateTest, RefusalsAreRelayIdentityUnavailableAndANewerFormatIsUpdateNeeded) {
  const Case* version_2 = nullptr;
  const Case* forged = nullptr;
  for (const Case& c : Fresh()) {
    version_2 = c.name == "version_2" ? &c : version_2;
    forged = c.name == "one_signature" ? &c : forged;
  }
  ASSERT_TRUE(version_2 && forged);
  const RelayCertificateCheck newer = ReceiveRelayCertificate(version_2->certificate, Founding().Id(), kNowMs);
  EXPECT_TRUE(AppError::IsNewerFormat(newer.ToError()));
  const RelayCertificateCheck refused = ReceiveRelayCertificate(forged->certificate, Founding().Id(), kNowMs);
  const Error error = refused.ToError();
  EXPECT_FALSE(AppError::IsNewerFormat(error));
  EXPECT_EQ(AppError::CategoryOf(error), ErrorCategory::Network);
  EXPECT_EQ(static_cast<Err::Network>(error.code), Err::Network::RelayIdentityUnavailable);
  EXPECT_NE(error.message.find("relay_certificate_forged"), std::string::npos) << error.message;
}

TEST(RelayCertificateTest, NotBase64OrEmptyIsMalformed) {
  EXPECT_EQ(ReceiveRelayCertificateB64("", Founding().Id(), kNowMs).verdict, RelayCertificateVerdict::Malformed);
  EXPECT_EQ(ReceiveRelayCertificateB64("not base64!", Founding().Id(), kNowMs).verdict,
            RelayCertificateVerdict::Malformed);
  EXPECT_EQ(ReceiveRelayCertificate(ByteVector{}, Founding().Id(), kNowMs).verdict, RelayCertificateVerdict::Older)
      << "no version byte: missing, as the receiver's order reads it";
}

TEST(RelayCertificateTest, CertificateOriginsAreTheCardsBoundsWithPlainHttpToo) {
  EXPECT_TRUE(IsCertificateOrigin("https://relay.example/api/relay"));
  EXPECT_TRUE(IsCertificateOrigin("http://127.0.0.1:8787/api/relay")) << "the local stack";
  EXPECT_TRUE(IsCertificateOrigin("HTTP://127.0.0.1:8787/api/relay"));
  EXPECT_FALSE(IsCertificateOrigin("relay.example"));
  EXPECT_FALSE(IsCertificateOrigin("https://relay.example/api/relay?x=1"));
  EXPECT_FALSE(IsCertificateOrigin("https://relay.example/api/relay#Qm"));
  EXPECT_FALSE(IsCertificateOrigin("ftp://relay.example/api/relay"));
  EXPECT_FALSE(IsCertificateOrigin("https://" + std::string(130, 'a')));
}

TEST(RelayCertificateTest, ListsComparesOriginsAsRelaysDo) {
  const RelayCertificate certificate =
      ReceiveRelayCertificate(Fresh()[0].certificate, RelayId{}, kNowMs).certificate.value();
  EXPECT_TRUE(certificate.Lists("https://relay.example/api/relay"));
  EXPECT_TRUE(certificate.Lists("HTTPS://Relay.Example:443/api/relay/"));
  EXPECT_TRUE(certificate.Lists("https://relay-b.example/api/relay"));
  EXPECT_FALSE(certificate.Lists("https://relay.example"));
  EXPECT_FALSE(certificate.Lists(""));
}

TEST(RelayCertificateTest, TheHighestIsKept) {
  const RelayCertificate one = ReceiveRelayCertificate(Fresh()[0].certificate, RelayId{}, kNowMs).certificate.value();
  const RelayCertificate two = ReceiveRelayCertificate(Fresh()[1].certificate, RelayId{}, kNowMs).certificate.value();
  EXPECT_EQ(CompareRelayCertificates(nullptr, one), RelayCertificateOffer::Adopted);
  EXPECT_EQ(CompareRelayCertificates(&one, two), RelayCertificateOffer::Adopted);
  EXPECT_EQ(CompareRelayCertificates(&two, one), RelayCertificateOffer::Lower);
  EXPECT_EQ(CompareRelayCertificates(&two, two), RelayCertificateOffer::Same);
}

} // namespace
} // namespace pbr
