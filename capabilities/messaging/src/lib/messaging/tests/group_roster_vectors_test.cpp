// The group objects' frozen vectors (release-compatibility SIGNED_FORMATS § 2.5, § 6): roster statement v4, dissolved
// v4, owner transfer v2 and roster change v2 — the signed bytes of fixed inputs (an empty area, one unknown
// non-critical entry), signed fixtures, and the receiver's verdict on each refusal (data/group_roster_vectors.json).
// The app tells only "update needed" (AppError::NewerFormat) from a refusal, so a group fixture's verdict is
// `accepted`, `needs_update` or `refused`. PP_WRITE_GROUP_ROSTER_VECTORS=<path> writes a fresh file.

#include "lib/messaging/GroupChange.h"
#include "lib/messaging/GroupRosterStatement.h"

#include "common/ValueJson.h"
#include "common/directory/IAccountSigningAccess.h"
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

namespace grs = group_roster_statement;
namespace grc = group_change;

const VectorFile kFile{PP_GROUP_ROSTER_VECTORS_PATH, "PP_WRITE_GROUP_ROSTER_VECTORS"};
const std::set<std::string> kSignatureKeys = {"owner_signature", "signature"};
constexpr char kFormat[] = "pp-browser group roster vectors v1";
constexpr char kGroup[] = "group:vectors";
constexpr char kDevice[] = "12D3KooWVectorDevice";

/** An account from a fixed seed, signing as the app does (a bare signature, typed by the object's signer). */
class SeededAccount final : public IAccountSigningAccess {
public:
  explicit SeededAccount(const uint8_t fill)
      : keys_(MlDsa::GenerateKeyPairFromSeed(ByteVector(pp::kMlDsa65SeedBytes, fill)).value()),
        id_(AccountIdFromMlDsaPublicKey(keys_.public_key).value()) {}
  Roe<std::string> GetAccountId() const override { return id_; }
  Roe<std::string> SignAccountBytes(const AccountSignBytes& bytes) const override {
    return Base64Encode(MlDsa::Sign(keys_.secret_key, bytes).value());
  }
  const std::string& id() const { return id_; }
  std::string KeyB64() const { return Base64Encode(keys_.public_key); }
  const MlDsaKeyPair& keys() const { return keys_; }

private:
  MlDsaKeyPair keys_;
  std::string id_;
};

SeededAccount& Owner() {
  static SeededAccount account(0x01);
  return account;
}
SeededAccount& FormerOwner() {
  static SeededAccount account(0x02);
  return account;
}
SeededAccount& DeviceKey() {
  static SeededAccount account(0x03);
  return account;
}

ExtArea OneEntry(const uint16_t type) {
  return ExtArea::FromEntries({{.type = type, .value = {0xca, 0xfe}}}).value();
}

GroupOwnerTransfer Transfer(const ExtArea& ext = {}) {
  GroupOwnerTransfer t = grs::SignTransfer(FormerOwner(), FormerOwner().KeyB64(), kGroup, Owner().id(), 2).value();
  if (!ext.empty()) {
    t.ext = ext;
    auto signature = FormerOwner().SignAccountBytes(grs::TransferSignBytes(kGroup, t)).value();
    t.signature_b64 = Base64Encode(TypedSignature(Base64Decode(signature).value()).value());
  }
  return t;
}

GroupRosterStatement Statement(const ExtArea& ext = {}, const bool dissolved = false,
                               const std::string& role_text = "") {
  GroupRosterStatement draft;
  draft.group_id = kGroup;
  draft.roster_epoch = 3;
  GroupRosterStatementMember member{.identity = "account:member", .role = MemberRole::Member, .joined_at = 20};
  member.role_text = role_text;
  draft.members = {{.identity = Owner().id(), .role = MemberRole::Owner, .joined_at = 10},
                   {.identity = FormerOwner().id(), .role = MemberRole::Admin, .joined_at = 5},
                   member};
  draft.preset = GroupPreset::Managed;
  draft.title = "Vectors";
  draft.title_version = 1;
  draft.owner_chain = {Transfer()};
  draft.dissolved = dissolved;
  draft.ext = ext;
  return grs::SignStatement(Owner(), Owner().KeyB64(), draft).value();
}

GroupChange Change(const ExtArea& ext = {}) {
  GroupChange change;
  change.group_id = kGroup;
  change.base_epoch = 3;
  change.signer = DeviceKey().id();
  change.signer_device = kDevice;
  change.at = 1767225600000;
  change.kind = GroupChangeKind::Rename;
  change.title = "Renamed";
  change.title_version = 2;
  change.ext = ext;
  return grc::Sign(change, [](const ByteVector& bytes) { return DeviceKey().SignAccountBytes(bytes); }).value();
}

AccountCardDevice Device() {
  return AccountCardDevice{.peer_id = kDevice, .signing_public_key_b64 = DeviceKey().KeyB64()};
}

/** The app's verdict: accepted, needs_update (NewerFormat), or refused. */
std::string Verdict(const Roe<void>& result) {
  if (result) {
    return "accepted";
  }
  return AppError::IsNewerFormat(result.error()) ? "needs_update" : "refused";
}

std::string StatementVerdict(const Object& json) {
  auto statement = grs::FromJson(json);
  if (!statement) {
    return Verdict(Roe<void>(statement.error()));
  }
  return Verdict(grs::Verify(*statement, FormerOwner().id()));
}

std::string ChangeVerdict(const Object& json) {
  auto change = grc::FromJson(json);
  if (!change) {
    return Verdict(Roe<void>(change.error()));
  }
  return Verdict(grc::VerifyDeviceSignature(*change, Device()));
}

Object Row(const std::string& name, const std::string& object_key, const Object& object, const std::string& verdict,
           const std::string& why) {
  Object row;
  row.set("name", name);
  row.set(object_key, ObjectValue(object));
  row.set("verdict", verdict);
  row.set("why", why);
  return row;
}

Object BytesRow(const std::string& name, const ByteVector& bytes) {
  Object row;
  row.set("name", name);
  row.set("sign_bytes_hex", BytesToHex(bytes));
  return row;
}

Object Set(Object json, const std::string& key, Value value) {
  json.set(key, std::move(value));
  return json;
}

Object WithByte0(Object json, const std::string& key, const uint8_t id) {
  ByteVector blob = Base64Decode(json.getString(key).value()).value();
  blob[0] = id;
  json.set(key, Base64Encode(blob));
  return json;
}

Object BuildVectors() {
  const Object statement = grs::ToJson(Statement());
  std::vector<Value> statements;
  const auto add = [&](const std::string& name, const Object& json, const std::string& verdict,
                       const std::string& why) {
    statements.push_back(ObjectValue(Row(name, "statement", json, verdict, why)));
  };
  add("valid", statement, "accepted", "the owner's, with a chain from the owner the reader knew");
  add("unknown_non_critical_entry", grs::ToJson(Statement(OneEntry(0x0042))), "accepted", "ignored and kept");
  add("unknown_role", grs::ToJson(Statement({}, false, "moderator")), "accepted",
      "the role is signed as received: it verifies, then reads as member");
  add("dissolved", grs::ToJson(Statement({}, true)), "accepted", "statement_version 4, the dissolved domain");
  add("statement_version_5", Set(statement, "statement_version", int64_t{5}), "needs_update", "read first");
  add("statement_version_3", Set(statement, "statement_version", int64_t{3}), "refused", "made by an older app");
  add("unknown_critical_entry", grs::ToJson(Statement(OneEntry(0x8042))), "needs_update",
      "it verifies, then the entry is read");
  add("unknown_owner_key_algorithm", WithByte0(statement, "owner_public_key", 2), "needs_update", "not verified");
  add("unsigned_area_added", Set(statement, kExtB64Key, std::string(Base64Encode(OneEntry(0x8042).EncodeEntries()))),
      "refused", "the area is in the signed bytes");
  add("dissolved_flag_added", Set(statement, "dissolved", true), "refused", "another domain: the signature fails");
  {
    Object newer_transfer = statement;
    const Array* chain = statement.getArray("owner_chain");
    Object transfer = *pp::common::asObject(chain->elements.front());
    transfer.set("transfer_version", int64_t{3});
    newer_transfer.set("owner_chain", ArrayValue({ObjectValue(transfer)}));
    add("transfer_version_3", newer_transfer, "needs_update", "a nested object's version, read first");
  }
  {
    GroupRosterStatement s = Statement();
    s.owner_chain = {Transfer(OneEntry(0x8042))};
    s = grs::SignStatement(Owner(), Owner().KeyB64(), s).value();
    add("transfer_unknown_critical_entry", grs::ToJson(s), "needs_update", "the transfer verifies, then its entry");
  }

  const Object change = grc::ToJson(Change());
  std::vector<Value> changes;
  const auto add_change = [&](const std::string& name, const Object& json, const std::string& verdict,
                              const std::string& why) {
    changes.push_back(ObjectValue(Row(name, "change", json, verdict, why)));
  };
  add_change("valid", change, "accepted", "signed by the device on the signer's card");
  add_change("unknown_non_critical_entry", grc::ToJson(Change(OneEntry(0x0042))), "accepted", "ignored and kept");
  add_change("change_version_3", Set(change, "change_version", int64_t{3}), "needs_update", "read first");
  add_change("change_version_1", Set(change, "change_version", int64_t{1}), "refused", "made by an older app");
  add_change("unknown_kind", Set(change, "change", std::string("pin")), "needs_update",
             "its body's layout is a newer app's");
  add_change("unknown_critical_entry", grc::ToJson(Change(OneEntry(0x8042))), "needs_update",
             "it verifies, then the entry is read");
  add_change("unknown_signature_algorithm", WithByte0(change, "signature", 2), "needs_update", "not verified");
  add_change("title_edited",
             Set(change, "body", ObjectValue(Set(*change.getObject("body"), "title", std::string("Edited")))),
             "refused", "the signature covers it");

  Object root;
  root.set("format", std::string(kFormat));
  root.set("statement_layout",
           std::string("\"pp-browser:group-roster-statement\" (dissolved: \"pp-browser:group-dissolved\") 00 u8(4) "
                       "then G018's fields, strings u32 BE length, then u64(len(entries)) entries"));
  root.set("transfer_layout", std::string("\"pp-browser:group-owner-transfer\" 00 u8(2) group from to u64(epoch) "
                                          "u64(len(entries)) entries"));
  root.set("change_layout", std::string("\"pp-browser:group-roster-change\" 00 u8(2) group u64(base_epoch) signer "
                                        "device u64(at) kind body u64(len(entries)) entries; id = BLAKE2b-256"));
  root.set("known_owner", FormerOwner().id());
  root.set("device", ObjectValue(Set(Set(Object{}, "peer_id", std::string(kDevice)), "signing_public_key_b64",
                                     DeviceKey().KeyB64())));
  root.set(
      "sign_bytes",
      ArrayValue(
          {ObjectValue(BytesRow("statement_empty_area", grs::SignBytes(Statement()))),
           ObjectValue(BytesRow("statement_unknown_non_critical_entry", grs::SignBytes(Statement(OneEntry(0x0042))))),
           ObjectValue(BytesRow("dissolved_empty_area", grs::SignBytes(Statement({}, true)))),
           ObjectValue(BytesRow("transfer_empty_area", grs::TransferSignBytes(kGroup, Transfer()))),
           ObjectValue(BytesRow("transfer_unknown_non_critical_entry",
                                grs::TransferSignBytes(kGroup, Transfer(OneEntry(0x0042))))),
           ObjectValue(BytesRow("change_empty_area", grc::SignBytes(Change()))),
           ObjectValue(BytesRow("change_unknown_non_critical_entry", grc::SignBytes(Change(OneEntry(0x0042)))))}));
  Object id;
  id.set("change_id", grc::Id(Change()));
  root.set("change_id", ObjectValue(id));
  root.set("statements", ArrayValue(std::move(statements)));
  root.set("changes", ArrayValue(std::move(changes)));
  return root;
}

TEST(GroupRosterVectorsTest, WriteAFreshFileWhenAsked) {
  if (!kFile.WriteRequested()) {
    GTEST_SKIP() << kFile.SkipReason();
  }
  ASSERT_TRUE(kFile.Write(BuildVectors()));
}

// RC020-VEC: the committed file is what this build makes, but for the hedged signatures (the sign bytes and the
// change id are frozen).
TEST(GroupRosterVectorsTest, TheCommittedFileIsWhatTheBuildersMake) {
  EXPECT_EQ(VectorFile::Text(WithoutKeys(kFile.Read(), kSignatureKeys)),
            VectorFile::Text(WithoutKeys(BuildVectors(), kSignatureKeys)))
      << kFile.RegenerateHint();
}

// RC020-ORDER: each committed fixture gets its verdict from the receiver's order.
TEST(GroupRosterVectorsTest, EachFixtureGetsItsVerdict) {
  const Object committed = kFile.Read();
  for (const auto& [list, key] :
       {std::pair<std::string, std::string>{"statements", "statement"}, {"changes", "change"}}) {
    const Array* rows = committed.getArray(list);
    ASSERT_NE(rows, nullptr) << list;
    for (const Value& item : rows->elements) {
      const Object& row = *pp::common::asObject(item);
      const Object& json = *row.getObject(key);
      EXPECT_EQ(key == "statement" ? StatementVerdict(json) : ChangeVerdict(json),
                row.getString("verdict").value_or(""))
          << list << " " << row.getString("name").value_or("");
    }
  }
}

// What a receiver keeps it writes back as it came: the area, the role as received, the version fields.
TEST(GroupRosterVectorsTest, AnAcceptedObjectWritesBackAsItCame) {
  for (const Object& json :
       {grs::ToJson(Statement(OneEntry(0x0042))), grs::ToJson(Statement({}, false, "moderator"))}) {
    auto statement = grs::FromJson(json);
    ASSERT_TRUE(statement) << statement.error().message;
    EXPECT_EQ(DumpJson(grs::ToJson(*statement)), DumpJson(json));
  }
  auto moderated = grs::FromJson(grs::ToJson(Statement({}, false, "moderator"))).value();
  EXPECT_EQ(moderated.members.back().role, MemberRole::Member) << "an unknown role reads as member";
  const Object change = grc::ToJson(Change(OneEntry(0x0042)));
  auto read = grc::FromJson(change);
  ASSERT_TRUE(read);
  EXPECT_EQ(DumpJson(grc::ToJson(*read)), DumpJson(change));
  EXPECT_EQ(grc::Id(*read), grc::Id(Change(OneEntry(0x0042))));
  EXPECT_NE(grc::Id(*read), grc::Id(Change())) << "the area is in the bytes the id hashes";
}

} // namespace
} // namespace pbr
