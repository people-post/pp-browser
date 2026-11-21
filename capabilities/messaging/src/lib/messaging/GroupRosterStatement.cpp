#include "lib/messaging/GroupRosterStatement.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/crypto/SignedReceive.h"
#include "foundation/crypto/TypedBlob.h"
#include "foundation/error/AppError.h"
#include "lib/people/CardKeys.h"
#include "common/ValueJson.h"

#include <algorithm>
#include <map>
#include <set>
#include "common/PbrCompat.h"

namespace pbr {
namespace group_roster_statement {
namespace {

// The signing domains (SIGNED_FORMATS § 2.5): the version is the byte after the domain, the same number as the JSON's
// `statement_version` / `transfer_version`. Nothing older is read (the signed-format cutover is a clean break).
constexpr const char kStatementDomain[] = "pp-browser:group-roster-statement";
constexpr const char kDissolvedDomain[] = "pp-browser:group-dissolved";
constexpr const char kTransferDomain[] = "pp-browser:group-owner-transfer";
constexpr const char kStatementVersionKey[] = "statement_version";
constexpr const char kTransferVersionKey[] = "transfer_version";
constexpr SignedVersions kStatementVersions{.floor = kStatementVersion, .current = kStatementVersion};

/** u32 big-endian length, then the bytes: no field can run into the next. */
void Put(ByteVector& out, const std::string& field) {
  const auto n = static_cast<uint32_t>(field.size());
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>(n >> shift));
  }
  out.insert(out.end(), field.begin(), field.end());
}

void Put(ByteVector& out, const uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>(value >> shift));
  }
}

bool IdentityLess(const GroupRosterStatementMember& a, const GroupRosterStatementMember& b) {
  return a.identity < b.identity;
}

void SortLists(GroupRosterStatement& statement) {
  std::sort(statement.members.begin(), statement.members.end(), IdentityLess);
  std::sort(statement.removed.begin(), statement.removed.end(),
            [](const GroupRemovedMember& a, const GroupRemovedMember& b) { return a.identity < b.identity; });
  std::sort(statement.links.begin(), statement.links.end(),
            [](const GroupInviteLink& a, const GroupInviteLink& b) { return a.link_id < b.link_id; });
  std::sort(statement.folded.begin(), statement.folded.end(),
            [](const GroupFoldedChange& a, const GroupFoldedChange& b) { return a.id < b.id; });
}

void PutMembers(ByteVector& out, const std::vector<GroupRosterStatementMember>& members) {
  Put(out, static_cast<uint64_t>(members.size()));
  for (const GroupRosterStatementMember& m : members) {
    Put(out, m.identity);
    Put(out, RoleText(m));
    Put(out, static_cast<uint64_t>(m.joined_at));
    Put(out, m.added_by);
  }
}

void PutRemoved(ByteVector& out, const std::vector<GroupRemovedMember>& removed) {
  Put(out, static_cast<uint64_t>(removed.size()));
  for (const GroupRemovedMember& r : removed) {
    Put(out, r.identity);
    Put(out, r.removal);
    Put(out, r.removed_by);
  }
}

void PutLinks(ByteVector& out, const std::vector<GroupInviteLink>& links) {
  Put(out, static_cast<uint64_t>(links.size()));
  for (const GroupInviteLink& l : links) {
    Put(out, l.link_id);
    Put(out, l.secret_hash);
    Put(out, l.maker);
    Put(out, static_cast<uint64_t>(l.expires_at));
    Put(out, l.max_uses);
    Put(out, l.uses);
  }
}

template <typename T, typename Key>
bool AllUnique(const std::vector<T>& items, Key key) {
  std::set<std::string> seen;
  for (const T& item : items) {
    const std::string& k = key(item);
    if (k.empty() || !seen.insert(k).second) {
      return false;
    }
  }
  return true;
}

/** The G018 lists: removed accounts unique and not members, link ids unique, folded ids unique and above the floor. */
Roe<void> CheckLists(const GroupRosterStatement& statement) {
  if (!AllUnique(statement.removed, [](const GroupRemovedMember& r) -> const std::string& { return r.identity; })) {
    return Error("roster statement: empty or repeated removed account");
  }
  for (const GroupRemovedMember& r : statement.removed) {
    if (statement.Lists(r.identity)) {
      return Error("roster statement: a removed account is also a member");
    }
  }
  if (!AllUnique(statement.links, [](const GroupInviteLink& l) -> const std::string& { return l.link_id; })) {
    return Error("roster statement: empty or repeated link id");
  }
  if (!AllUnique(statement.folded, [](const GroupFoldedChange& f) -> const std::string& { return f.id; })) {
    return Error("roster statement: empty or repeated folded change");
  }
  if (statement.change_floor_epoch > statement.roster_epoch) {
    return Error("roster statement: change floor above its epoch");
  }
  for (const GroupFoldedChange& f : statement.folded) {
    if (f.base_epoch < statement.change_floor_epoch) {
      return Error("roster statement: a folded change below the change floor");
    }
  }
  return {};
}

/** One typed key and signature pair to verify (step 3 reads them, step 4 checks them). */
struct SignedBy {
  std::string identity;
  TypedRead key;
  TypedRead signature;
};

/** Step 3: `key_b64` and `signature_b64` as typed blobs (an unknown id is NewerFormat). */
Roe<SignedBy> ReadSignedBy(const std::string& identity, const std::string& key_b64, const std::string& signature_b64) {
  SignedBy by{.identity = identity,
              .key = ReadTypedBlobB64(pp::BlobKind::kSignaturePublicKey, key_b64, "roster statement key"),
              .signature = ReadTypedBlobB64(pp::BlobKind::kSignature, signature_b64, "roster statement signature")};
  if (!by.key.ok()) {
    return by.key.ToError();
  }
  if (!by.signature.ok()) {
    return by.signature.ToError();
  }
  return by;
}

/** Step 4: the raw key hashes to `by.identity`, and the signature over `bytes` is that key's. */
Roe<void> CheckSignature(const SignedBy& by, const ByteVector& bytes) {
  auto account = AccountIdFromMlDsaPublicKey(by.key.raw);
  if (!account || *account != by.identity) {
    return Error("roster statement: key is not " + by.identity + "'s");
  }
  if (auto verified = VerifyTyped(by.key, bytes, by.signature); !verified) {
    return Error("roster statement: signature does not verify for " + by.identity);
  }
  return {};
}

/** Step 1 of a nested object: its own version, `current` only. */
Roe<void> CheckNestedVersion(const Object& json, const char* key, const int64_t current, const std::string& what) {
  const std::optional<int64_t> version = OutsideVersionOfJson(json, key);
  if (!version || *version < current) {
    return Error(what + " made by an older app (no " + key + " " + std::to_string(current) + ")");
  }
  if (*version > current) {
    return AppError::NewerFormat(what + " version " + std::to_string(*version) + ": made by a newer app");
  }
  return {};
}

Object RemovedToJson(const GroupRemovedMember& removed) {
  Object row;
  row.set("identity", removed.identity);
  row.set("removal", removed.removal);
  row.set("by", removed.removed_by);
  return row;
}

Object LinkToJson(const GroupInviteLink& link) {
  Object row;
  row.set("link_id", link.link_id);
  row.set("secret_hash", link.secret_hash);
  row.set("maker", link.maker);
  row.setJsonUInt("expires_at", static_cast<uint64_t>(std::max<int64_t>(link.expires_at, 0)));
  if (link.max_uses > 0) {
    row.setJsonUInt("max_uses", link.max_uses);
  }
  if (link.uses > 0) {
    row.setJsonUInt("uses", link.uses);
  }
  return row;
}

/** The G018 fields: title, removed, links, folded, the change floor (each omitted when empty). */
void PutOpenGroupJson(Object& json, const GroupRosterStatement& statement) {
  if (!statement.title.empty() || statement.title_version > 0) {
    json.set("title", statement.title);
    json.setJsonUInt("title_version", statement.title_version);
  }
  std::vector<Value> removed;
  for (const GroupRemovedMember& r : statement.removed) {
    removed.push_back(ObjectValue(RemovedToJson(r)));
  }
  if (!removed.empty()) {
    json.set("removed", ArrayValue(std::move(removed)));
  }
  std::vector<Value> links;
  for (const GroupInviteLink& l : statement.links) {
    links.push_back(ObjectValue(LinkToJson(l)));
  }
  if (!links.empty()) {
    json.set("links", ArrayValue(std::move(links)));
  }
  std::vector<Value> folded;
  for (const GroupFoldedChange& f : statement.folded) {
    Object row;
    row.set("id", f.id);
    row.setJsonUInt("base_epoch", f.base_epoch);
    folded.push_back(ObjectValue(std::move(row)));
  }
  if (!folded.empty()) {
    json.set("folded", ArrayValue(std::move(folded)));
  }
  if (statement.change_floor_epoch > 0) {
    json.setJsonUInt("change_floor_epoch", statement.change_floor_epoch);
  }
}

/** Every element of `key` as an object, or an error naming `what`. */
Roe<std::vector<const Object*>> ObjectRows(const Object& json, const std::string& key, const std::string& what) {
  std::vector<const Object*> rows;
  if (const Array* items = json.getArray(key)) {
    for (const Value& item : items->elements) {
      const Object* row = asObject(item);
      if (!row) {
        return Error("roster statement: " + what + " is not an object");
      }
      rows.push_back(row);
    }
  }
  return rows;
}

Roe<void> ReadOpenGroupJson(const Object& json, GroupRosterStatement& statement) {
  statement.title = json.getString("title").value_or("");
  statement.title_version = json.getNonNegInt("title_version").value_or(0);
  auto removed = ObjectRows(json, "removed", "removed entry");
  auto links = ObjectRows(json, "links", "link");
  if (!removed) {
    return removed.error();
  }
  if (!links) {
    return links.error();
  }
  for (const Object* row : *removed) {
    statement.removed.push_back({row->getString("identity").value_or(""), row->getString("removal").value_or(""),
                                 row->getString("by").value_or("")});
  }
  for (const Object* row : *links) {
    statement.links.push_back({row->getString("link_id").value_or(""), row->getString("secret_hash").value_or(""),
                               row->getString("maker").value_or(""),
                               static_cast<int64_t>(row->getNonNegInt("expires_at").value_or(0)),
                               row->getNonNegInt("max_uses").value_or(0), row->getNonNegInt("uses").value_or(0)});
  }
  auto folded = ObjectRows(json, "folded", "folded change");
  if (!folded) {
    return folded.error();
  }
  for (const Object* row : *folded) {
    statement.folded.push_back({row->getString("id").value_or(""), row->getNonNegInt("base_epoch").value_or(0)});
  }
  statement.change_floor_epoch = json.getNonNegInt("change_floor_epoch").value_or(0);
  return {};
}

} // namespace

std::string RoleText(const GroupRosterStatementMember& member) {
  if (!member.role_text.empty() && MemberRoleFromString(member.role_text) == member.role) {
    return member.role_text;
  }
  return MemberRoleToString(member.role);
}

std::string PresetText(const GroupRosterStatement& statement) {
  if (!statement.preset_text.empty() && GroupPresetFromString(statement.preset_text) == statement.preset) {
    return statement.preset_text;
  }
  return GroupPresetToString(statement.preset);
}

ByteVector SignBytes(const GroupRosterStatement& statement) {
  GroupRosterStatement sorted = statement;
  SortLists(sorted);
  ByteVector fields;
  Put(fields, sorted.group_id);
  Put(fields, sorted.roster_epoch);
  Put(fields, sorted.owner_identity);
  PutMembers(fields, sorted.members);
  Put(fields, PresetText(sorted));
  Put(fields, sorted.title);
  Put(fields, sorted.title_version);
  PutRemoved(fields, sorted.removed);
  PutLinks(fields, sorted.links);
  Put(fields, static_cast<uint64_t>(sorted.folded.size()));
  for (const GroupFoldedChange& f : sorted.folded) {
    Put(fields, f.id);
    Put(fields, f.base_epoch);
  }
  Put(fields, sorted.change_floor_epoch);
  return SignedBytesWriter(statement.dissolved ? kDissolvedDomain : kStatementDomain, kStatementVersion)
      .Raw(fields)
      .Finish(statement.ext);
}

ByteVector TransferSignBytes(const std::string& group_id, const GroupOwnerTransfer& transfer) {
  ByteVector fields;
  Put(fields, group_id);
  Put(fields, transfer.from_identity);
  Put(fields, transfer.to_identity);
  Put(fields, transfer.roster_epoch);
  return SignedBytesWriter(kTransferDomain, kTransferVersion).Raw(fields).Finish(transfer.ext);
}

Roe<GroupRosterStatement> SignStatement(IAccountSigningAccess& signer, const std::string& public_key_b64,
                                        GroupRosterStatement draft) {
  auto account = signer.GetAccountId();
  if (!account) {
    return account.error();
  }
  auto typed_key = TypedKeyB64(pp::BlobKind::kSignaturePublicKey, public_key_b64);
  if (!typed_key) {
    return typed_key.error();
  }
  SortLists(draft);
  draft.owner_identity = *account;
  draft.owner_public_key_b64 = std::move(typed_key.value());
  auto signature = signer.SignAccountBytes(SignBytes(draft));
  if (!signature) {
    return signature.error();
  }
  auto typed = TypedSignatureB64(*signature);
  if (!typed) {
    return typed.error();
  }
  draft.owner_signature_b64 = std::move(typed.value());
  return draft;
}

Roe<GroupRosterStatement> Sign(IAccountSigningAccess& signer, const std::string& public_key_b64,
                               const std::string& group_id, const uint64_t roster_epoch,
                               std::vector<GroupRosterStatementMember> members,
                               std::vector<GroupOwnerTransfer> owner_chain, const bool dissolved,
                               const GroupPreset preset) {
  GroupRosterStatement statement;
  statement.group_id = group_id;
  statement.roster_epoch = roster_epoch;
  statement.members = std::move(members);
  statement.owner_chain = std::move(owner_chain);
  statement.dissolved = dissolved;
  statement.preset = preset;
  return SignStatement(signer, public_key_b64, std::move(statement));
}

Roe<GroupOwnerTransfer> SignTransfer(IAccountSigningAccess& signer, const std::string& public_key_b64,
                                     const std::string& group_id, const std::string& to_identity,
                                     const uint64_t roster_epoch) {
  auto account = signer.GetAccountId();
  if (!account) {
    return account.error();
  }
  auto typed_key = TypedKeyB64(pp::BlobKind::kSignaturePublicKey, public_key_b64);
  if (!typed_key) {
    return typed_key.error();
  }
  GroupOwnerTransfer transfer;
  transfer.from_identity = *account;
  transfer.from_public_key_b64 = std::move(typed_key.value());
  transfer.to_identity = to_identity;
  transfer.roster_epoch = roster_epoch;
  auto signature = signer.SignAccountBytes(TransferSignBytes(group_id, transfer));
  if (!signature) {
    return signature.error();
  }
  auto typed = TypedSignatureB64(*signature);
  if (!typed) {
    return typed.error();
  }
  transfer.signature_b64 = std::move(typed.value());
  return transfer;
}

Roe<void> Verify(const GroupRosterStatement& statement, const std::string& known_owner) {
  // Steps 1–5 of the receiver's order: the statement and every transfer of its chain are signed objects.
  std::optional<SignedBy> owner;
  std::vector<SignedBy> chain;
  const SignedReceipt receipt = ReceiveSigned({
      .what = statement.dissolved ? "group dissolved statement" : "roster statement",
      .outside_version = kStatementVersion, // FromJson read `statement_version`: a statement is this version
      .versions = kStatementVersions,
      .parse = [&]() -> Roe<void> {
        if (statement.group_id.empty() || statement.owner_identity.empty()) {
          return Error("missing group or owner");
        }
        return {};
      },
      .algorithms = [&]() -> Roe<void> {
        auto by = ReadSignedBy(statement.owner_identity, statement.owner_public_key_b64, statement.owner_signature_b64);
        if (!by) {
          return by.error();
        }
        owner = std::move(by.value());
        for (const GroupOwnerTransfer& t : statement.owner_chain) {
          auto from = ReadSignedBy(t.from_identity, t.from_public_key_b64, t.signature_b64);
          if (!from) {
            return from.error();
          }
          chain.push_back(std::move(from.value()));
        }
        return {};
      },
      .verify = [&]() -> Roe<void> {
        if (auto ok = CheckSignature(*owner, SignBytes(statement)); !ok) {
          return ok.error();
        }
        for (size_t i = 0; i < chain.size(); ++i) {
          if (auto ok = CheckSignature(chain[i], TransferSignBytes(statement.group_id, statement.owner_chain[i]));
              !ok) {
            return ok.error();
          }
        }
        return {};
      },
      .ext = &statement.ext,
  });
  if (!receipt.accepted()) {
    return receipt.ToError();
  }
  // Step 5 for the transfers, now that each is known to be its signer's.
  for (const GroupOwnerTransfer& t : statement.owner_chain) {
    if (const auto unknown = t.ext.FirstUnknownCritical({})) {
      return AppError::NewerFormat("owner transfer: unknown critical extension " + std::to_string(*unknown));
    }
  }
  // Step 6: the terms.
  std::set<std::string> seen;
  size_t owners = 0;
  bool owner_listed = false;
  for (const GroupRosterStatementMember& m : statement.members) {
    if (m.identity.empty() || !seen.insert(m.identity).second) {
      return Error("roster statement: empty or repeated member");
    }
    if (m.role == MemberRole::Owner) {
      ++owners;
      owner_listed = m.identity == statement.owner_identity;
    }
  }
  if (owners != 1 || !owner_listed) {
    return Error("roster statement: its signer must be its one owner");
  }
  if (auto lists = CheckLists(statement); !lists) {
    return lists.error();
  }
  // The chain: each transfer starting where the last ended, epochs rising, the last ending at this statement's owner.
  uint64_t last_epoch = 0;
  for (size_t i = 0; i < statement.owner_chain.size(); ++i) {
    const GroupOwnerTransfer& t = statement.owner_chain[i];
    if (i > 0 && t.from_identity != statement.owner_chain[i - 1].to_identity) {
      return Error("roster statement: owner chain is broken");
    }
    if (t.roster_epoch <= last_epoch || t.roster_epoch > statement.roster_epoch) {
      return Error("roster statement: owner chain epochs out of order");
    }
    last_epoch = t.roster_epoch;
  }
  if (!statement.owner_chain.empty() && statement.owner_chain.back().to_identity != statement.owner_identity) {
    return Error("roster statement: owner chain does not end at its owner");
  }
  if (known_owner.empty() || known_owner == statement.owner_identity) {
    return {};
  }
  for (const GroupOwnerTransfer& t : statement.owner_chain) {
    if (t.from_identity == known_owner) {
      return {};
    }
  }
  return Error("roster statement: signed by " + statement.owner_identity + ", not by the owner we know (" +
               known_owner + ") or one it handed the group to");
}

bool OwnerHandedOn(const std::vector<GroupOwnerTransfer>& known, const GroupRosterStatement& candidate) {
  for (const GroupOwnerTransfer& away : known) {
    if (away.from_identity != candidate.owner_identity || away.roster_epoch > candidate.roster_epoch) {
      continue;
    }
    const bool back = std::any_of(known.begin(), known.end(), [&](const GroupOwnerTransfer& again) {
      return again.to_identity == candidate.owner_identity && again.roster_epoch > away.roster_epoch &&
             again.roster_epoch <= candidate.roster_epoch;
    });
    if (!back) {
      return true;
    }
  }
  return false;
}

std::optional<GroupRosterStatement> Pick(const std::vector<GroupRosterStatement>& valid) {
  // An owner that handed the group on by epoch e (and did not get it back) is no longer its owner at e.
  std::vector<GroupOwnerTransfer> known;
  for (const GroupRosterStatement& s : valid) {
    known.insert(known.end(), s.owner_chain.begin(), s.owner_chain.end());
  }
  const GroupRosterStatement* best = nullptr;
  for (const GroupRosterStatement& s : valid) {
    if (OwnerHandedOn(known, s)) {
      continue;
    }
    if (!best || s.roster_epoch > best->roster_epoch) {
      best = &s;
    }
  }
  if (!best) {
    return std::nullopt;
  }
  return *best;
}

Object TransferToJson(const GroupOwnerTransfer& transfer) {
  Object json;
  json.set(kTransferVersionKey, int64_t{kTransferVersion});
  json.set("from", transfer.from_identity);
  json.set("from_public_key", transfer.from_public_key_b64);
  json.set("to", transfer.to_identity);
  json.setJsonUInt("roster_epoch", transfer.roster_epoch);
  WriteExtB64(json, transfer.ext);
  json.set("signature", transfer.signature_b64);
  return json;
}

Roe<GroupOwnerTransfer> TransferFromJson(const Object& json) {
  if (auto version = CheckNestedVersion(json, kTransferVersionKey, kTransferVersion, "owner transfer"); !version) {
    return version.error();
  }
  GroupOwnerTransfer transfer;
  transfer.from_identity = json.getString("from").value_or("");
  transfer.from_public_key_b64 = json.getString("from_public_key").value_or("");
  transfer.to_identity = json.getString("to").value_or("");
  transfer.roster_epoch = json.getNonNegInt("roster_epoch").value_or(0);
  transfer.signature_b64 = json.getString("signature").value_or("");
  if (transfer.from_identity.empty() || transfer.to_identity.empty() || transfer.signature_b64.empty()) {
    return Error("owner transfer: missing field");
  }
  auto ext = ReadExtB64(json);
  if (!ext) {
    return Error("owner transfer: " + ext.error().message);
  }
  transfer.ext = std::move(ext.value());
  auto by = ReadSignedBy(transfer.from_identity, transfer.from_public_key_b64, transfer.signature_b64);
  if (!by) {
    return by.error();
  }
  return transfer;
}

Object ToJson(const GroupRosterStatement& statement) {
  Object json;
  json.set(kStatementVersionKey, int64_t{kStatementVersion});
  json.set("group_id", statement.group_id);
  json.setJsonUInt("roster_epoch", statement.roster_epoch);
  std::vector<Value> members;
  for (const GroupRosterStatementMember& m : statement.members) {
    Object row;
    row.set("identity", m.identity);
    row.set("role", RoleText(m));
    row.setJsonUInt("joined_at", static_cast<uint64_t>(std::max<int64_t>(m.joined_at, 0)));
    if (!m.added_by.empty()) {
      row.set("added_by", m.added_by);
    }
    members.push_back(ObjectValue(std::move(row)));
  }
  json.set("members", ArrayValue(std::move(members)));
  json.set("preset", PresetText(statement));
  PutOpenGroupJson(json, statement);
  json.set("owner", statement.owner_identity);
  json.set("owner_public_key", statement.owner_public_key_b64);
  if (!statement.owner_chain.empty()) {
    std::vector<Value> chain;
    for (const GroupOwnerTransfer& t : statement.owner_chain) {
      chain.push_back(ObjectValue(TransferToJson(t)));
    }
    json.set("owner_chain", ArrayValue(std::move(chain)));
  }
  WriteExtB64(json, statement.ext);
  json.set("owner_signature", statement.owner_signature_b64);
  if (statement.dissolved) {
    json.set("dissolved", true);
  }
  return json;
}

Roe<GroupRosterStatement> FromJson(const Object& json) {
  // 1. The version, a dissolved statement's too, before anything else is read.
  if (auto version = CheckNestedVersion(json, kStatementVersionKey, kStatementVersion, "roster statement"); !version) {
    return version.error();
  }
  // 2. The fields (the role and preset as received) and the extension area.
  GroupRosterStatement statement;
  statement.group_id = json.getString("group_id").value_or("");
  statement.roster_epoch = json.getNonNegInt("roster_epoch").value_or(0);
  statement.owner_identity = json.getString("owner").value_or("");
  statement.owner_public_key_b64 = json.getString("owner_public_key").value_or("");
  statement.owner_signature_b64 = json.getString("owner_signature").value_or("");
  statement.dissolved = json.getIf<bool>("dissolved").value_or(false);
  statement.preset_text = json.getString("preset").value_or("open");
  statement.preset = GroupPresetFromString(statement.preset_text);
  const Array* members = json.getArray("members");
  if (statement.group_id.empty() || statement.owner_identity.empty() || !members) {
    return Error("roster statement: missing field");
  }
  for (const Value& item : members->elements) {
    const Object* row = asObject(item);
    if (!row) {
      return Error("roster statement: member is not an object");
    }
    GroupRosterStatementMember m;
    m.identity = row->getString("identity").value_or("");
    m.role_text = row->getString("role").value_or("member");
    m.role = MemberRoleFromString(m.role_text);
    m.joined_at = static_cast<int64_t>(row->getNonNegInt("joined_at").value_or(0));
    m.added_by = row->getString("added_by").value_or("");
    statement.members.push_back(std::move(m));
  }
  if (auto open = ReadOpenGroupJson(json, statement); !open) {
    return open.error();
  }
  if (const Array* chain = json.getArray("owner_chain")) {
    for (const Value& item : chain->elements) {
      const Object* row = asObject(item);
      if (!row) {
        return Error("roster statement: owner transfer is not an object");
      }
      auto transfer = TransferFromJson(*row);
      if (!transfer) {
        return transfer.error();
      }
      statement.owner_chain.push_back(std::move(*transfer));
    }
  }
  auto ext = ReadExtB64(json);
  if (!ext) {
    return Error("roster statement: " + ext.error().message);
  }
  statement.ext = std::move(ext.value());
  // 3. The owner's typed key and signature (each transfer's were read with it).
  if (auto by = ReadSignedBy(statement.owner_identity, statement.owner_public_key_b64, statement.owner_signature_b64);
      !by) {
    return by.error();
  }
  return statement;
}

} // namespace group_roster_statement

bool GroupRosterStatement::Lists(const std::string& identity) const {
  return std::any_of(members.begin(), members.end(),
                     [&](const GroupRosterStatementMember& m) { return m.identity == identity; });
}

bool GroupRosterStatement::Folded(const std::string& id) const {
  return std::any_of(folded.begin(), folded.end(), [&](const GroupFoldedChange& f) { return f.id == id; });
}

} // namespace pbr
