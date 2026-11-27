// Locker record v2 and the state's version (release-compatibility SIGNED_FORMATS § 2.6, INVENTORY T7): the receiver's
// order for a record, a newer app's copy skipped and not written over, writers that keep what they do not know, and
// the frozen vectors (pp-specs' vectors/account_locker_vectors.json) the relay's locker store (pp-relay-apis `crypto/locker.ts`)
// reads byte for byte. PP_WRITE_ACCOUNT_LOCKER_VECTORS=<path> writes a fresh file.

#include "lib/people/AccountLocker.h"

#include "common/ValueJson.h"
#include "common/tests/VectorFile.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/RecoveryPhrase.h"
#include "foundation/crypto/RecoverySeed.h"
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

const VectorFile kFile{PP_ACCOUNT_LOCKER_VECTORS_PATH, "PP_WRITE_ACCOUNT_LOCKER_VECTORS"};
const std::set<std::string> kSignatureKeys = {"signature_b64"};
constexpr char kFormat[] = "pp-browser account locker vectors v1";

LockerKeys KeysOf(const uint8_t fill) {
  return account_locker::KeysFrom(DeriveRecoveryKeys(ByteVector(kRecoverySeedBytes, fill)).value()).value();
}

ExtArea OneEntry(const uint16_t type) {
  return ExtArea::FromEntries({{.type = type, .value = {0xca, 0xfe}}}).value();
}

/** A record of `keys`' locker over fixed sealed bytes (an AEAD blob's shape: id 1, then 48 bytes), signed. */
LockerSnapshot FixedRecord(const LockerKeys& keys, const uint64_t version, const ExtArea& ext) {
  LockerSnapshot snapshot;
  snapshot.locker_id = keys.locker_id;
  snapshot.version = version;
  snapshot.signing_public_key = TypedSigningPublicKey(keys.signing_public_key).value();
  snapshot.sealed = ByteVector(49, 0x5e);
  snapshot.sealed[0] = 1;
  snapshot.ext = ext;
  snapshot.signature =
      SignTyped(keys.signing_secret_key, account_locker::SignBytes(snapshot.locker_id, version, snapshot.sealed, ext))
          .value();
  return snapshot;
}

Object SignBytesCase(const std::string& name, const ExtArea& ext) {
  ByteVector locker_id(32);
  for (size_t i = 0; i < locker_id.size(); ++i) {
    locker_id[i] = static_cast<uint8_t>(i);
  }
  const ByteVector sealed = {0x01, 0xaa, 0xbb};
  Object row;
  row.set("name", name);
  row.set("locker_id", BytesToHex(locker_id));
  row.set("version", int64_t{7});
  row.set("sealed_b64", Base64Encode(sealed));
  WriteExtB64(row, ext);
  row.set("sign_bytes_hex", BytesToHex(account_locker::SignBytes(locker_id, 7, sealed, ext)));
  return row;
}

Object Fixture(const std::string& name, const Object& record, const SignedVerdict verdict, const std::string& why) {
  Object row;
  row.set("name", name);
  row.set("record", ObjectValue(record));
  row.set("verdict", std::string(SignedVerdictName(verdict)));
  row.set("why", why);
  return row;
}

Object WithByte0(Object record, const std::string& key, const uint8_t id) {
  ByteVector blob = Base64Decode(record.getString(key).value()).value();
  blob[0] = id;
  record.set(key, Base64Encode(blob));
  return record;
}

Object BuildVectors() {
  const LockerKeys keys = KeysOf(0x01);
  const LockerKeys other = KeysOf(0x02);
  const Object valid = account_locker::RecordToJson(FixedRecord(keys, 3, ExtArea{}));

  std::vector<Value> fixtures;
  fixtures.push_back(ObjectValue(Fixture("valid", valid, SignedVerdict::Accepted, "signed by the locker key")));
  fixtures.push_back(ObjectValue(Fixture("unknown_non_critical_entry",
                                         account_locker::RecordToJson(FixedRecord(keys, 3, OneEntry(0x0042))),
                                         SignedVerdict::Accepted, "ignored and kept: stored as received")));
  Object newer = valid;
  newer.set("record_version", int64_t{3});
  newer.set("sealed_b64", std::string("not base64 at all"));
  fixtures.push_back(ObjectValue(
      Fixture("record_version_3", newer, SignedVerdict::NeedsUpdate, "read first: nothing else is read or verified")));
  Object older = valid;
  older.set("record_version", int64_t{1});
  fixtures.push_back(ObjectValue(Fixture("record_version_1", older, SignedVerdict::Older, "below the floor")));
  Object missing = valid;
  missing.erase("record_version");
  fixtures.push_back(ObjectValue(Fixture("no_record_version", missing, SignedVerdict::Older, "made by an older app")));
  fixtures.push_back(ObjectValue(Fixture("unknown_critical_entry",
                                         account_locker::RecordToJson(FixedRecord(keys, 3, OneEntry(0x8042))),
                                         SignedVerdict::NeedsUpdate, "it verifies, then the entry is read")));
  fixtures.push_back(ObjectValue(Fixture("unknown_key_algorithm", WithByte0(valid, "signing_public_key_b64", 2),
                                         SignedVerdict::NeedsUpdate, "signature algorithm id 2: not verified")));
  fixtures.push_back(ObjectValue(Fixture("unknown_signature_algorithm", WithByte0(valid, "signature_b64", 2),
                                         SignedVerdict::NeedsUpdate, "signature algorithm id 2: not verified")));
  fixtures.push_back(ObjectValue(Fixture("unknown_aead_algorithm", WithByte0(valid, "sealed_b64", 2),
                                         SignedVerdict::NeedsUpdate, "AEAD algorithm id 2: not verified")));
  Object added_area = valid;
  added_area.set(kExtB64Key, Base64Encode(OneEntry(0x8042).EncodeEntries()));
  fixtures.push_back(ObjectValue(Fixture("unsigned_area_added", added_area, SignedVerdict::Forged,
                                         "the area is in the signed bytes: an unsigned one never says update")));
  Object bad_area = valid;
  bad_area.set(kExtB64Key, std::string("AAAA"));
  fixtures.push_back(
      ObjectValue(Fixture("malformed_area", bad_area, SignedVerdict::Malformed, "three bytes: no entry")));
  Object bumped = valid;
  bumped.set("version", int64_t{4});
  fixtures.push_back(ObjectValue(Fixture("version_edited", bumped, SignedVerdict::Forged, "the signature covers it")));
  Object squatter = account_locker::RecordToJson(FixedRecord(other, 3, ExtArea{}));
  squatter.set("locker_id", BytesToHex(keys.locker_id));
  fixtures.push_back(ObjectValue(
      Fixture("key_not_the_lockers", squatter, SignedVerdict::Forged, "its raw key does not hash to the locker id")));

  Object root;
  root.set("format", std::string(kFormat));
  root.set("domain", std::string(kLockerRecordDomain));
  root.set("record_version", int64_t{kLockerRecordVersion});
  root.set("layout", std::string("domain 00 u8(2) LenBytes(locker_id) u64(version) LenBytes(sealed) "
                                 "u64(len(entries)) entries; locker_id = BLAKE2b-256(raw signing key)"));
  root.set("sign_bytes", ArrayValue({ObjectValue(SignBytesCase("empty_area", ExtArea{})),
                                     ObjectValue(SignBytesCase("unknown_non_critical_entry", OneEntry(0x0042)))}));
  root.set("fixtures", ArrayValue(std::move(fixtures)));
  return root;
}

TEST(AccountLockerVectorsTest, WriteAFreshFileWhenAsked) {
  if (!kFile.WriteRequested()) {
    GTEST_SKIP() << kFile.SkipReason();
  }
  ASSERT_TRUE(kFile.Write(BuildVectors()));
}

// RC020-VEC: the committed file is what this build makes, but for the hedged signatures.
TEST(AccountLockerVectorsTest, TheCommittedFileIsWhatTheBuildersMake) {
  EXPECT_EQ(VectorFile::Text(WithoutKeys(kFile.Read(), kSignatureKeys)),
            VectorFile::Text(WithoutKeys(BuildVectors(), kSignatureKeys)))
      << kFile.RegenerateHint();
}

TEST(AccountLockerVectorsTest, TheSignedBytesAreTheRecordsFields) {
  const Object committed = kFile.Read();
  const Array* cases = committed.getArray("sign_bytes");
  ASSERT_NE(cases, nullptr);
  ASSERT_EQ(cases->elements.size(), 2u);
  for (const Value& item : cases->elements) {
    const Object& row = *pp::common::asObject(item);
    const ByteVector bytes = account_locker::SignBytes(
        HexToBytes(row.getString("locker_id").value()).value(), row.getNonNegInt("version").value(),
        Base64Decode(row.getString("sealed_b64").value()).value(), ReadExtB64(row).value());
    EXPECT_EQ(BytesToHex(bytes), row.getString("sign_bytes_hex").value_or("")) << row.getString("name").value_or("");
    EXPECT_EQ(SignedBytesVersion(bytes), kLockerRecordVersion);
  }
}

// RC020-ORDER: each committed fixture gets its verdict from the receiver's order, as a provider runs it.
TEST(AccountLockerVectorsTest, EachFixtureGetsItsVerdict) {
  const Object committed = kFile.Read();
  const Array* fixtures = committed.getArray("fixtures");
  ASSERT_NE(fixtures, nullptr);
  for (const Value& item : fixtures->elements) {
    const Object& row = *pp::common::asObject(item);
    const std::string name = row.getString("name").value_or("");
    LockerSnapshot snapshot;
    const SignedReceipt receipt = account_locker::ReceiveRecord(*row.getObject("record"), &snapshot);
    EXPECT_EQ(SignedVerdictName(receipt.verdict), row.getString("verdict").value_or(""))
        << name << ": " << receipt.detail;
    EXPECT_EQ(receipt.needs_update(), AppError::IsNewerFormat(receipt.ToError())) << name;
    if (receipt.accepted()) {
      EXPECT_EQ(DumpJson(account_locker::RecordToJson(snapshot)), DumpJson(*row.getObject("record")))
          << name << ": stored and served as received";
    }
  }
}

// ---- The app's side: Fetch reads the record, then the state. ----

TEST(AccountLockerRecordTest, ARecordJsonRoundTripsAndNamesItsVersionFirst) {
  const LockerKeys keys = KeysOf(0x11);
  const LockerSnapshot snapshot = account_locker::Seal(AccountLockerState{.nickname = "a"}, keys, 5).value();
  const Object json = account_locker::RecordToJson(snapshot);
  EXPECT_EQ(json.entries().begin()->first, "record_version");
  EXPECT_EQ(json.getIf<int64_t>("record_version"), 2);
  EXPECT_FALSE(json.contains(kExtB64Key)) << "an empty area is left out";
  auto back = account_locker::RecordFromJson(json);
  ASSERT_TRUE(back) << back.error().message;
  EXPECT_EQ(*back, snapshot);
  EXPECT_TRUE(account_locker::VerifyOwner(*back));
  EXPECT_EQ(account_locker::Open(*back, keys)->nickname, "a");
}

TEST(AccountLockerRecordTest, ANewerRecordIsUpdateNeededBeforeAnythingElseIsRead) {
  Object newer;
  newer.set("record_version", int64_t{3});
  EXPECT_TRUE(AppError::IsNewerFormat(account_locker::RecordFromJson(newer).error()));
  Object older = account_locker::RecordToJson(account_locker::Seal(AccountLockerState{}, KeysOf(0x11), 1).value());
  older.set("record_version", int64_t{1});
  ASSERT_FALSE(account_locker::RecordFromJson(older));
  EXPECT_FALSE(AppError::IsNewerFormat(account_locker::RecordFromJson(older).error()));
}

// SIGNED_FORMATS § 2.6: PickNewest skips a newer app's copy and says so; the device then does not publish over it.
TEST(AccountLockerRecordTest, PickNewestSkipsANewerAppsCopyAndNotesIt) {
  const LockerKeys keys = KeysOf(0x11);
  const LockerSnapshot v3 = account_locker::Seal(AccountLockerState{.nickname = "mine"}, keys, 3).value();
  LockerSnapshot newer_record = account_locker::Seal(AccountLockerState{}, keys, 9).value();
  newer_record.record_version = 3;
  LockerSnapshot critical = account_locker::Seal(AccountLockerState{}, keys, 8).value();
  critical.ext = OneEntry(0x8001);
  critical.signature =
      SignTyped(keys.signing_secret_key,
                account_locker::SignBytes(critical.locker_id, critical.version, critical.sealed, critical.ext))
          .value();
  EXPECT_TRUE(AppError::IsNewerFormat(account_locker::Open(critical, keys).error()));

  const account_locker::PickedSnapshot picked = account_locker::PickNewest({v3, newer_record, critical}, keys);
  ASSERT_TRUE(picked.newest);
  EXPECT_EQ(picked.newest->version, 3u) << "the newest this app reads";
  EXPECT_TRUE(picked.newer_format);
  EXPECT_FALSE(account_locker::PickNewest({v3}, keys).newer_format);
  EXPECT_FALSE(account_locker::PickNewest({newer_record}, keys).newest);
}

// T7: `state_version` is read first once a snapshot opens; a newer one is "update needed", not a broken state.
TEST(AccountLockerRecordTest, TheStatesVersionIsReadFirst) {
  EXPECT_EQ(account_locker::StateToJson(AccountLockerState{}).rfind(R"({"state_version":1,)", 0), 0u);
  auto newer = account_locker::StateFromJson(R"({"state_version":2,"contacts":"anything at all"})");
  ASSERT_FALSE(newer);
  EXPECT_TRUE(AppError::IsNewerFormat(newer.error()));
  for (const char* older : {R"({"nickname":"a"})", R"({"state_version":0})", R"({"state_version":"1"})"}) {
    auto read = account_locker::StateFromJson(older);
    ASSERT_FALSE(read) << older;
    EXPECT_FALSE(AppError::IsNewerFormat(read.error())) << older;
  }
}

// T7: a writer keeps what it does not know — the state's unknown keys and every row's unknown fields come back on the
// row of the same key, through a merge and a publish — so a newer device's additions survive an older device's write.
TEST(AccountLockerRecordTest, WritersKeepUnknownKeysAndRowFields) {
  const std::string stored_json =
      R"({"state_version":1,"nickname":"alice","future_top":{"x":1},)"
      R"("contacts":[{"id":"c1","local":{"display_name":"Bob","trust":"unknown","tint":"blue"},)"
      R"("remote":{"nickname":"bob","ids":[],"endpoints":[],"multiaddrs":[],"pronouns":"they"},"overrides":{},)"
      R"("pinned":true}],)"
      R"("groups":[{"group_id":"group:a","title":"A","owner":"account:alice","roster_epoch":2,"title_version":0,)"
      R"("policy":"","members":[{"identity":"account:alice","role":"owner","joined_at":1,"contact_id":"",)"
      R"("muted":true}],"roster_statement":"","emoji":"x"}],)"
      R"("card_floors":[{"account_id":"account:bob","issued_at":5,"seen_on":"phone"}],)"
      R"("grants":[{"grant_version":2,"recipient_account_id":"account:bob","sender_account_id":"account:alice",)"
      R"("issued_at":5,"expires_at":9000000000000,"signature":"AXNpZw==","relay_hint":"r"}],)"
      R"("relays":{"list":[{"origin":"https://a.example","relay_id":"relay:CgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgo",)"
      R"("weight":3}],"card_issued_at":7,"policy":"p"},)"
      R"("providers":[],"contact_clock":[{"account_id":"account:bob","at":4,"present":true,"digest":"d","dev":"x"}],)"
      R"("device_names":[{"peer_id":"peer:a","name":"Phone","at":4,"icon":"star"}]})";
  auto stored = account_locker::StateFromJson(stored_json);
  ASSERT_TRUE(stored) << stored.error().message;

  // Read and written back as it is: every unknown field returns.
  const std::string rewritten = account_locker::StateToJson(*stored);
  for (const char* kept : {R"("future_top":{"x":1})", R"("tint":"blue")", R"("pronouns":"they")", R"("pinned":true)",
                           R"("muted":true)", R"("emoji":"x")", R"("seen_on":"phone")", R"("relay_hint":"r")",
                           R"("weight":3)", R"("policy":"p")", R"("dev":"x")", R"("icon":"star")"}) {
    EXPECT_NE(rewritten.find(kept), std::string::npos) << kept;
  }

  // Merged with this device's state and published: the rows that stay carry their fields; a row this device dropped
  // takes its fields with it.
  AccountLockerState ours = *stored;
  ours.kept = Object{};
  ours.nickname = "alice2";
  ours.extras.groups[0].title = "A renamed";
  ours.extras.groups[0].roster_epoch = 3; // ours is newer: it replaces the stored row
  ours.extras.card_floors.clear();
  const std::string published = account_locker::StateToJson(account_locker::MergeForPublish(ours, *stored));
  EXPECT_NE(published.find(R"("emoji":"x")"), std::string::npos) << "the group row ours replaced keeps its field";
  EXPECT_NE(published.find(R"("future_top":{"x":1})"), std::string::npos);
  EXPECT_NE(published.find(R"("seen_on":"phone")"), std::string::npos) << "the stored floor is merged back";
  AccountLockerState without_relays = ours;
  without_relays.extras.relays = {};
  without_relays.kept = stored->kept;
  EXPECT_EQ(account_locker::StateToJson(without_relays).find(R"("weight":3)"), std::string::npos)
      << "no relays written: none of their fields either";

  // A known field this build writes is its own: a kept value never overrides it.
  EXPECT_NE(published.find(R"("nickname":"alice2")"), std::string::npos);
  EXPECT_NE(published.find(R"("title":"A renamed")"), std::string::npos);
}

} // namespace
} // namespace pbr
