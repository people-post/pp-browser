// Broadcast's signed formats at the signed-format cutover (release-compatibility SIGNED_FORMATS § 2.9): the join
// ticket v2 and the peer-announce tip v2. The receiver's order (docs/contracts/SIGNED_EXTENSIONS.md) for each, and
// their frozen vector files (broadcast_join_ticket_vectors.json, peer_announce_tip_vectors.json): the signed bytes of
// fixed inputs, signed fixtures that must verify, and the refusals with their verdicts.
// PP_WRITE_BROADCAST_JOIN_TICKET_VECTORS=<path> / PP_WRITE_PEER_ANNOUNCE_TIP_VECTORS=<path> write fresh files.

#include "lib/broadcast/BroadcastJoinTicket.h"
#include "lib/broadcast/PeerAnnounceCodec.h"

#include "common/tests/VectorFile.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/crypto/SignedReceive.h"
#include "foundation/crypto/TypedBlob.h"
#include "foundation/error/AppError.h"

#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

using pp::ExtArea;
using test::VectorFile;
using test::WithoutKeys;

const VectorFile kTicketFile{PP_BROADCAST_JOIN_TICKET_VECTORS_PATH, "PP_WRITE_BROADCAST_JOIN_TICKET_VECTORS"};
const VectorFile kTipFile{PP_PEER_ANNOUNCE_TIP_VECTORS_PATH, "PP_WRITE_PEER_ANNOUNCE_TIP_VECTORS"};
const std::set<std::string> kSignatureKeys{"signature_b64"};

constexpr uint8_t kPublisherSeedByte = 0x21;

MlDsaKeyPair PublisherKeys() {
  return MlDsa::GenerateKeyPairFromSeed(ByteVector(pp::kMlDsa65SeedBytes, kPublisherSeedByte)).value();
}

ExtArea NonCritical() {
  return ExtArea::FromEntries({{.type = 0x0042, .value = {0x01, 0x02}}}).value();
}

ExtArea Critical() {
  return ExtArea::FromEntries({{.type = 0x8042, .value = {}}}).value();
}

ByteVector Repeated(const uint8_t byte, const size_t n) {
  return ByteVector(n, byte);
}

Object ParsedJson(const Roe<std::string>& json) {
  return ParseObject(json.value()).value();
}

/** Sets one field of a wire JSON object (a refusal's case). */
Object With(Object json, const std::string& key, Value value) {
  json.set(key, std::move(value));
  return json;
}

/** The typed signature with its algorithm id replaced (an algorithm this build does not know). */
std::string WithSignatureAlgorithm(const std::string& signature_b64, const uint8_t alg_id) {
  ByteVector bytes = Base64Decode(signature_b64).value();
  bytes[0] = alg_id;
  return Base64Encode(bytes);
}

Object Case(const std::string& name, const std::string& note, const std::string& object_key, Object object,
            const SignedVerdict expect, const std::optional<ByteVector>& sign_bytes = std::nullopt) {
  Object row;
  row.set("name", name);
  row.set("note", note);
  row.set(object_key, std::move(object));
  if (sign_bytes) {
    row.set("sign_bytes_hex", BytesToHex(*sign_bytes));
  }
  row.set("expect", std::string(SignedVerdictName(expect)));
  return row;
}

Object FileHeader(const std::string& about, const char* domain, const int version) {
  const MlDsaKeyPair keys = PublisherKeys();
  Object file;
  file.set("about", about);
  file.set("domain", std::string(domain));
  file.set("schema_version", int64_t{version});
  file.set("publisher_seed_hex", BytesToHex(Repeated(kPublisherSeedByte, pp::kMlDsa65SeedBytes)));
  file.set("publisher_public_key_b64", Base64Encode(TypedSigningPublicKey(keys.public_key).value()));
  return file;
}

/** The publisher's raw key, read back from a file's typed `publisher_public_key_b64`. */
ByteVector FilePublisherKey(const Object& file) {
  const TypedRead key = ReadTypedBlobB64(pp::BlobKind::kSignaturePublicKey,
                                         file.getString("publisher_public_key_b64").value_or(""), "publisher key");
  EXPECT_TRUE(key.ok()) << key.detail;
  return key.raw;
}

// ---- The join ticket ----

BroadcastJoinTicket BaseTicket() {
  BroadcastJoinTicket ticket;
  ticket.publisher_peer_id = "12D3KooWPublisher";
  ticket.program_id = "show-1";
  ticket.join_handle = "live:show-1";
  ticket.viewer_peer_id = "12D3KooWViewer";
  ticket.media_epoch = 1;
  ticket.media_key_id = "mk:00112233445566778899aabbccddeeff";
  ticket.key_material_b64 = Base64Encode(Repeated(0x40, 32));
  ticket.hop_peer_id = "12D3KooWHop";
  ticket.expires_at_ms = 2'000'000'000'000;
  return ticket;
}

/** A wrapped key as MediaKeyWrap makes one (typed AEAD id `alg_id`, nonce 24, ciphertext 32 + tag 16), fixed bytes. */
std::string FixedWrappedKey(const uint8_t alg_id) {
  ByteVector blob{alg_id};
  const ByteVector nonce = Repeated(0xAA, 24);
  const ByteVector sealed = Repeated(0xBB, 48);
  blob.insert(blob.end(), nonce.begin(), nonce.end());
  blob.insert(blob.end(), sealed.begin(), sealed.end());
  return Base64Encode(blob);
}

BroadcastJoinTicket Signed(BroadcastJoinTicket ticket) {
  ticket.signature_b64 =
      Base64Encode(SignTyped(PublisherKeys().secret_key, BroadcastJoinTicketSignBytes(ticket).value()).value());
  return ticket;
}

Object TicketJson(const BroadcastJoinTicket& ticket) {
  return ParsedJson(EncodeBroadcastJoinTicketJson(ticket));
}

Object BuildTicketVectors() {
  Object file = FileHeader(
      "pp-browser broadcast join ticket v2 (release-compatibility SIGNED_FORMATS § 2.9): "
      "sign_bytes_hex is frozen; signatures are hedged, so each accepted fixture must verify",
      kBroadcastJoinTicketDomain, kBroadcastJoinTicketSchemaVersion);
  std::vector<Value> cases;
  const auto add = [&](const std::string& name, const std::string& note, Object ticket, const SignedVerdict expect,
                       const std::optional<ByteVector>& sign_bytes = std::nullopt) {
    cases.push_back(ObjectValue(Case(name, note, "ticket", std::move(ticket), expect, sign_bytes)));
  };

  const BroadcastJoinTicket plain = Signed(BaseTicket());
  add("key_material", "the open path: raw key material, an empty area", TicketJson(plain), SignedVerdict::Accepted,
      BroadcastJoinTicketSignBytes(plain).value());

  BroadcastJoinTicket wrapped = BaseTicket();
  wrapped.key_material_b64.clear();
  wrapped.wrapped_key_b64 = FixedWrappedKey(1);
  wrapped.hop_peer_id.clear();
  wrapped = Signed(wrapped);
  add("wrapped_key_no_hop", "a typed AEAD wrapped key (xchacha20-poly1305, id 1); no hop: signed as \"\"",
      TicketJson(wrapped), SignedVerdict::Accepted, BroadcastJoinTicketSignBytes(wrapped).value());

  BroadcastJoinTicket extended = BaseTicket();
  extended.ext = NonCritical();
  extended = Signed(extended);
  add("unknown_non_critical_entry", "an unknown non-critical entry (0x0042): verifies, ignored and kept",
      TicketJson(extended), SignedVerdict::Accepted, BroadcastJoinTicketSignBytes(extended).value());

  add("version_plus_one", "schema_version 3: update needed before anything is verified",
      With(TicketJson(plain), "schema_version", Value(int64_t{3})), SignedVerdict::NeedsUpdate);
  add("version_minus_one", "schema_version 1 (today's text form): made by an older app",
      With(TicketJson(plain), "schema_version", Value(int64_t{1})), SignedVerdict::Older);
  Object no_version = TicketJson(plain);
  no_version.erase("schema_version");
  add("no_version", "no schema_version: made by an older app", no_version, SignedVerdict::Older);

  BroadcastJoinTicket critical = BaseTicket();
  critical.ext = Critical();
  critical = Signed(critical);
  add("unknown_critical_entry", "an unknown critical entry (0x8042), signed: verifies, then update needed",
      TicketJson(critical), SignedVerdict::NeedsUpdate, BroadcastJoinTicketSignBytes(critical).value());

  add("unknown_signature_algorithm", "the signature's algorithm id 2 (unknown): update needed, not a forgery",
      With(TicketJson(plain), "signature_b64", Value(WithSignatureAlgorithm(plain.signature_b64, 2))),
      SignedVerdict::NeedsUpdate);

  BroadcastJoinTicket later_aead = wrapped;
  later_aead.wrapped_key_b64 = FixedWrappedKey(2);
  later_aead = Signed(later_aead);
  add("unknown_wrapped_key_algorithm", "the wrapped key's AEAD id 2 (unknown): update needed", TicketJson(later_aead),
      SignedVerdict::NeedsUpdate);

  add("edited_field", "program_id changed after signing: a forgery",
      With(TicketJson(plain), "program_id", Value(std::string("show-2"))), SignedVerdict::Forged);
  add("unsigned_area", "an ext_b64 the publisher did not sign: a forgery",
      With(TicketJson(plain), "ext_b64", Value(std::string("AEIAAAACAQI="))), SignedVerdict::Forged);
  add("malformed_area", "ext_b64 that is not an extension area: malformed",
      With(TicketJson(plain), "ext_b64", Value(std::string("AAAA"))), SignedVerdict::Malformed);
  add(
      "no_key", "neither wrapped_key_b64 nor key_material_b64: malformed",
      [&] {
        Object json = TicketJson(plain);
        json.erase("key_material_b64");
        return json;
      }(),
      SignedVerdict::Malformed);

  file.set("cases", ArrayValue(std::move(cases)));
  return file;
}

// ---- The peer-announce tip ----

PeerAnnounceTip BaseTip() {
  PeerAnnounceTip tip;
  tip.peer_id = "12D3KooWPublisher";
  tip.topic_id = std::string(64, 'a');
  tip.program_id = "show-1";
  tip.state = PeerAnnounceState::Live;
  tip.seq = 7;
  tip.epoch = 1;
  tip.created_at_ms = 1'700'000'000'000;
  tip.join_handle = "live:show-1";
  tip.body = "live now";
  return tip;
}

PeerAnnounceTip Signed(PeerAnnounceTip tip) {
  return SignPeerAnnounceTip(std::move(tip), PublisherKeys().secret_key).value();
}

Object TipJson(const PeerAnnounceTip& tip) {
  return ParsedJson(EncodePeerAnnounceTipJson(tip));
}

Object BuildTipVectors() {
  Object file = FileHeader(
      "pp-browser peer-announce tip v2 (release-compatibility SIGNED_FORMATS § 2.9): every field "
      "signed, empty when absent; sign_bytes_hex is frozen; each accepted fixture must verify",
      kPeerAnnounceTipDomain, kPeerAnnounceTipSchemaVersion);
  std::vector<Value> cases;
  const auto add = [&](const std::string& name, const std::string& note, Object tip, const SignedVerdict expect,
                       const std::optional<ByteVector>& sign_bytes = std::nullopt) {
    cases.push_back(ObjectValue(Case(name, note, "tip", std::move(tip), expect, sign_bytes)));
  };

  const PeerAnnounceTip plain = Signed(BaseTip());
  add("program_minimal", "a live program tip; every optional field empty and signed so", TipJson(plain),
      SignedVerdict::Accepted, PeerAnnounceTipSignBytes(plain).value());

  PeerAnnounceTip full = BaseTip();
  full.hop_peer_id = "12D3KooWHop1";
  full.l1_hop_peer_ids = {"12D3KooWHop1", "12D3KooWHop2"};
  full.video_levels = {1, 3};
  full.kind = kPeerAnnounceKindLiveChat;
  full.viewer_peer_id = "12D3KooWViewer";
  full.viewer_msg_id = "vm-1";
  full.body = "hello overlay";
  full.content_id_hex = std::string(64, 'c');
  full = Signed(full);
  add("every_field", "a live_chat tip with a hop, two L1 hops and two video levels", TipJson(full),
      SignedVerdict::Accepted, PeerAnnounceTipSignBytes(full).value());

  PeerAnnounceTip extended = BaseTip();
  extended.ext = NonCritical();
  extended = Signed(extended);
  add("unknown_non_critical_entry", "an unknown non-critical entry (0x0042): verifies, ignored and kept",
      TipJson(extended), SignedVerdict::Accepted, PeerAnnounceTipSignBytes(extended).value());

  add("version_plus_one", "schema_version 3: update needed before anything is verified",
      With(TipJson(plain), "schema_version", Value(int64_t{3})), SignedVerdict::NeedsUpdate);
  add("version_minus_one", "schema_version 1 (today's text form): made by an older app",
      With(TipJson(plain), "schema_version", Value(int64_t{1})), SignedVerdict::Older);

  PeerAnnounceTip critical = BaseTip();
  critical.ext = Critical();
  critical = Signed(critical);
  add("unknown_critical_entry", "an unknown critical entry (0x8042), signed: verifies, then update needed",
      TipJson(critical), SignedVerdict::NeedsUpdate, PeerAnnounceTipSignBytes(critical).value());

  add("unknown_signature_algorithm", "the signature's algorithm id 2 (unknown): update needed, not a forgery",
      With(TipJson(plain), "signature_b64", Value(WithSignatureAlgorithm(plain.signature_b64, 2))),
      SignedVerdict::NeedsUpdate);

  add("field_added", "a hop added to a tip signed without one: every field is signed, so a forgery",
      With(TipJson(plain), "hop_peer_id", Value(std::string("12D3KooWInserted"))), SignedVerdict::Forged);
  add("edited_field", "body changed after signing: a forgery",
      With(TipJson(plain), "body", Value(std::string("ended"))), SignedVerdict::Forged);
  add("video_level_out_of_range", "a video level 16 (levels are 1-15): malformed",
      With(TipJson(plain), "video_levels", ArrayValue({Value(int64_t{16})})), SignedVerdict::Malformed);

  file.set("cases", ArrayValue(std::move(cases)));
  return file;
}

using Receiver = std::function<SignedReceipt(const std::string& json, const ByteVector& key)>;

/** Every committed case gets its verdict; an accepted one's signed bytes are the frozen ones. */
void CheckCommittedCases(const VectorFile& vectors, const std::string& object_key, const Receiver& receive,
                         const std::function<std::string(const std::string& json)>& sign_bytes_hex) {
  const Object file = vectors.Read();
  const ByteVector key = FilePublisherKey(file);
  const Array* cases = file.getArray("cases");
  ASSERT_NE(cases, nullptr);
  ASSERT_FALSE(cases->elements.empty());
  for (const Value& item : cases->elements) {
    const Object* row = asObject(item);
    ASSERT_NE(row, nullptr);
    const std::string name = row->getString("name").value_or("");
    const Object* object = row->getObject(object_key);
    ASSERT_NE(object, nullptr) << name;
    const std::string json = DumpJson(*object);
    const SignedReceipt receipt = receive(json, key);
    EXPECT_EQ(SignedVerdictName(receipt.verdict), row->getString("expect").value_or(""))
        << name << ": " << receipt.detail;
    if (auto frozen = row->getString("sign_bytes_hex")) {
      EXPECT_EQ(sign_bytes_hex(json), *frozen) << name;
    }
  }
}

TEST(BroadcastSignedVectorsTest, WriteAFreshTicketFileWhenAsked) {
  if (!kTicketFile.WriteRequested()) {
    GTEST_SKIP() << kTicketFile.SkipReason();
  }
  ASSERT_TRUE(kTicketFile.Write(BuildTicketVectors()));
}

TEST(BroadcastSignedVectorsTest, WriteAFreshTipFileWhenAsked) {
  if (!kTipFile.WriteRequested()) {
    GTEST_SKIP() << kTipFile.SkipReason();
  }
  ASSERT_TRUE(kTipFile.Write(BuildTipVectors()));
}

TEST(BroadcastSignedVectorsTest, TheCommittedTicketFileIsWhatTheBuildersMake) {
  EXPECT_EQ(VectorFile::Text(WithoutKeys(kTicketFile.Read(), kSignatureKeys)),
            VectorFile::Text(WithoutKeys(BuildTicketVectors(), kSignatureKeys)))
      << kTicketFile.RegenerateHint();
}

TEST(BroadcastSignedVectorsTest, TheCommittedTipFileIsWhatTheBuildersMake) {
  EXPECT_EQ(VectorFile::Text(WithoutKeys(kTipFile.Read(), kSignatureKeys)),
            VectorFile::Text(WithoutKeys(BuildTipVectors(), kSignatureKeys)))
      << kTipFile.RegenerateHint();
}

TEST(BroadcastSignedVectorsTest, EveryCommittedTicketCaseGetsItsVerdict) {
  CheckCommittedCases(
      kTicketFile, "ticket",
      [](const std::string& json, const ByteVector& key) { return ReceiveBroadcastJoinTicketJson(json, key); },
      [](const std::string& json) {
        BroadcastJoinTicket ticket = DecodeBroadcastJoinTicketJson(json).value();
        return BytesToHex(BroadcastJoinTicketSignBytes(ticket).value());
      });
}

TEST(BroadcastSignedVectorsTest, EveryCommittedTipCaseGetsItsVerdict) {
  CheckCommittedCases(
      kTipFile, "tip",
      [](const std::string& json, const ByteVector& key) { return ReceivePeerAnnounceTipJson(json, key); },
      [](const std::string& json) {
        return BytesToHex(PeerAnnounceTipSignBytes(DecodePeerAnnounceTipJson(json).value()).value());
      });
}

// ---- The receiver's order, beyond the vectors ----

TEST(BroadcastSignedReceiveTest, TheTicketsSignedBytesStartWithTheDomainAndTheVersion) {
  const ByteVector bytes = BroadcastJoinTicketSignBytes(BaseTicket()).value();
  const std::string domain = kBroadcastJoinTicketDomain;
  ASSERT_GT(bytes.size(), domain.size() + 2);
  EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(domain.size())), domain);
  EXPECT_EQ(SignedBytesVersion(bytes), kBroadcastJoinTicketSchemaVersion);
  EXPECT_TRUE(IsSignedDomain(kBroadcastJoinTicketDomain));
  EXPECT_TRUE(IsSignedDomain(kPeerAnnounceTipDomain));
  EXPECT_EQ(SignedBytesVersion(PeerAnnounceTipSignBytes(BaseTip()).value()), kPeerAnnounceTipSchemaVersion);
}

// Step 1 before everything: a newer ticket is update needed even when nothing else about it holds.
TEST(BroadcastSignedReceiveTest, ANewerTicketIsUpdateNeededBeforeItsSignatureIsLookedAt) {
  BroadcastJoinTicket newer = BaseTicket();
  newer.schema_version = 3;
  newer.signature_b64 = "not a signature";
  newer.publisher_peer_id.clear();
  const auto verified = VerifyBroadcastJoinTicket(newer, PublisherKeys().public_key, 0);
  ASSERT_FALSE(verified);
  EXPECT_TRUE(AppError::IsNewerFormat(verified.error())) << verified.error().message;

  const auto decoded = DecodeBroadcastJoinTicketJson(R"({"schema_version":3,"anything":"else"})");
  ASSERT_FALSE(decoded);
  EXPECT_TRUE(AppError::IsNewerFormat(decoded.error())) << "the decoder compares the version first too";

  auto older = DecodeBroadcastJoinTicketJson(EncodeBroadcastJoinTicketJson(Signed(BaseTicket())).value());
  ASSERT_TRUE(older) << older.error().message;
  BroadcastJoinTicket v1 = *older;
  v1.schema_version = 1;
  const auto refused = VerifyBroadcastJoinTicket(v1, PublisherKeys().public_key, 0);
  ASSERT_FALSE(refused);
  EXPECT_FALSE(AppError::IsNewerFormat(refused.error())) << "older is a refusal, not update needed";
}

// Step 5 after step 4: an unknown critical entry nobody signed is a forgery, never "update needed".
TEST(BroadcastSignedReceiveTest, AnUnsignedCriticalEntryIsAForgeryNotUpdateNeeded) {
  BroadcastJoinTicket ticket = Signed(BaseTicket());
  ticket.ext = Critical();
  EXPECT_EQ(ReceiveBroadcastJoinTicket(ticket, PublisherKeys().public_key).verdict, SignedVerdict::Forged);

  PeerAnnounceTip tip = Signed(BaseTip());
  tip.ext = Critical();
  EXPECT_EQ(ReceivePeerAnnounceTip(tip, PublisherKeys().public_key).verdict, SignedVerdict::Forged);
}

// An unknown algorithm is update needed even under another publisher's key: the signature is not read.
TEST(BroadcastSignedReceiveTest, AnUnknownSignatureAlgorithmIsUpdateNeededWhoeverIsAsked) {
  const MlDsaKeyPair other = MlDsa::GenerateKeyPairFromSeed(ByteVector(pp::kMlDsa65SeedBytes, 0x99)).value();
  PeerAnnounceTip tip = Signed(BaseTip());
  tip.signature_b64 = WithSignatureAlgorithm(tip.signature_b64, 7);
  const auto verified = VerifyPeerAnnounceTip(tip, other.public_key);
  ASSERT_FALSE(verified);
  EXPECT_TRUE(AppError::IsNewerFormat(verified.error())) << verified.error().message;

  tip.signature_b64 = WithSignatureAlgorithm(tip.signature_b64, 0);
  EXPECT_EQ(ReceivePeerAnnounceTip(tip, PublisherKeys().public_key).verdict, SignedVerdict::Malformed) << "id 0";
}

// Unknown non-critical entries are kept: decoded, re-encoded and forwarded, the object still verifies.
TEST(BroadcastSignedReceiveTest, UnknownNonCriticalEntriesAreKeptThroughDecodeAndEncode) {
  BroadcastJoinTicket ticket = BaseTicket();
  ticket.ext = NonCritical();
  ticket = Signed(ticket);
  auto forwarded = DecodeBroadcastJoinTicketJson(EncodeBroadcastJoinTicketJson(ticket).value());
  ASSERT_TRUE(forwarded) << forwarded.error().message;
  EXPECT_EQ(forwarded->ext, NonCritical());
  EXPECT_TRUE(VerifyBroadcastJoinTicket(*forwarded, PublisherKeys().public_key, 0));

  PeerAnnounceTip tip = BaseTip();
  tip.ext = NonCritical();
  tip = Signed(tip);
  auto tip_forwarded = DecodePeerAnnounceTipJson(EncodePeerAnnounceTipJson(tip).value());
  ASSERT_TRUE(tip_forwarded) << tip_forwarded.error().message;
  EXPECT_EQ(tip_forwarded->ext, NonCritical());
  EXPECT_TRUE(VerifyPeerAnnounceTip(*tip_forwarded, PublisherKeys().public_key));
}

// A minted ticket's wrapped key (MediaKeyWrap) is the typed AEAD blob the signed bytes carry, and unwraps.
TEST(BroadcastSignedReceiveTest, AMintedWrappedTicketCarriesATypedAeadBlob) {
  const MlDsaKeyPair keys = PublisherKeys();
  BroadcastJoinTicketDraft draft;
  draft.publisher_peer_id = "12D3KooWPublisher";
  draft.program_id = "show-1";
  draft.join_handle = "live:show-1";
  draft.viewer_peer_id = "12D3KooWViewer";
  const ByteVector session_key = Repeated(0x33, 32);
  auto ticket = MintBroadcastJoinTicket(draft, Repeated(0x44, 32), keys.secret_key, &session_key);
  ASSERT_TRUE(ticket) << ticket.error().message;
  EXPECT_EQ(ticket->schema_version, kBroadcastJoinTicketSchemaVersion);
  const TypedRead wrapped = ReadTypedBlobB64(pp::BlobKind::kAeadSealed, ticket->wrapped_key_b64, "wrapped key");
  EXPECT_TRUE(wrapped.ok()) << wrapped.detail;
  auto key = ExtractBroadcastMediaKey(*ticket, keys.public_key, 0, "12D3KooWViewer", &session_key);
  ASSERT_TRUE(key) << key.error().message;
  EXPECT_EQ(key->key_bytes, Repeated(0x44, 32));
}

} // namespace
} // namespace pbr
