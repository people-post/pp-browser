#include "lib/messaging/GroupRosterStatement.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "common/ValueJson.h"

#include <algorithm>
#include <map>
#include <set>
#include "common/PbrCompat.h"

namespace pbr {
namespace group_roster_statement {
namespace {

constexpr const char kStatementTag[] = "pp-browser:group-roster-statement-v1";
constexpr const char kDissolvedTag[] = "pp-browser:group-dissolved-v1";
constexpr const char kTransferTag[] = "pp-browser:group-owner-transfer-v1";

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

Roe<void> CheckSignature(const std::string& identity, const std::string& public_key_b64, const ByteVector& bytes,
                         const std::string& signature_b64) {
  auto key = Base64Decode(public_key_b64);
  if (!key) {
    return Error("roster statement: bad key for " + identity);
  }
  auto account = AccountIdFromMlDsaPublicKey(*key);
  if (!account || *account != identity) {
    return Error("roster statement: key is not " + identity + "'s");
  }
  auto signature = Base64Decode(signature_b64);
  if (!signature) {
    return Error("roster statement: bad signature encoding");
  }
  auto ok = MlDsa::Verify(*key, bytes, *signature);
  if (!ok || !*ok) {
    return Error("roster statement: signature does not verify for " + identity);
  }
  return {};
}

} // namespace

ByteVector SignBytes(const GroupRosterStatement& statement) {
  std::vector<GroupRosterStatementMember> members = statement.members;
  std::sort(members.begin(), members.end(), IdentityLess);
  ByteVector out;
  Put(out, std::string(statement.dissolved ? kDissolvedTag : kStatementTag));
  Put(out, statement.group_id);
  Put(out, statement.roster_epoch);
  Put(out, statement.owner_identity);
  Put(out, static_cast<uint64_t>(members.size()));
  for (const GroupRosterStatementMember& m : members) {
    Put(out, m.identity);
    Put(out, MemberRoleToString(m.role));
    Put(out, static_cast<uint64_t>(m.joined_at));
  }
  return out;
}

ByteVector TransferSignBytes(const std::string& group_id, const GroupOwnerTransfer& transfer) {
  ByteVector out;
  Put(out, std::string(kTransferTag));
  Put(out, group_id);
  Put(out, transfer.from_identity);
  Put(out, transfer.to_identity);
  Put(out, transfer.roster_epoch);
  return out;
}

Roe<GroupRosterStatement> Sign(IAccountSigningAccess& signer, const std::string& public_key_b64,
                               const std::string& group_id, const uint64_t roster_epoch,
                               std::vector<GroupRosterStatementMember> members,
                               std::vector<GroupOwnerTransfer> owner_chain, const bool dissolved) {
  auto account = signer.GetAccountId();
  if (!account) {
    return account.error();
  }
  GroupRosterStatement statement;
  statement.group_id = group_id;
  statement.roster_epoch = roster_epoch;
  std::sort(members.begin(), members.end(), IdentityLess);
  statement.members = std::move(members);
  statement.owner_identity = *account;
  statement.owner_public_key_b64 = public_key_b64;
  statement.owner_chain = std::move(owner_chain);
  statement.dissolved = dissolved;
  auto signature = signer.SignAccountBytes(SignBytes(statement));
  if (!signature) {
    return signature.error();
  }
  statement.owner_signature_b64 = *signature;
  return statement;
}

Roe<GroupOwnerTransfer> SignTransfer(IAccountSigningAccess& signer, const std::string& public_key_b64,
                                     const std::string& group_id, const std::string& to_identity,
                                     const uint64_t roster_epoch) {
  auto account = signer.GetAccountId();
  if (!account) {
    return account.error();
  }
  GroupOwnerTransfer transfer;
  transfer.from_identity = *account;
  transfer.from_public_key_b64 = public_key_b64;
  transfer.to_identity = to_identity;
  transfer.roster_epoch = roster_epoch;
  auto signature = signer.SignAccountBytes(TransferSignBytes(group_id, transfer));
  if (!signature) {
    return signature.error();
  }
  transfer.signature_b64 = *signature;
  return transfer;
}

Roe<void> Verify(const GroupRosterStatement& statement, const std::string& known_owner) {
  if (statement.group_id.empty() || statement.owner_identity.empty()) {
    return Error("roster statement: missing group or owner");
  }
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
  if (auto ok = CheckSignature(statement.owner_identity, statement.owner_public_key_b64, SignBytes(statement),
                               statement.owner_signature_b64);
      !ok) {
    return ok.error();
  }
  // The chain: each transfer signed by the owner it moves from, each starting where the last ended,
  // epochs rising, the last ending at this statement's owner.
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
    if (auto ok = CheckSignature(t.from_identity, t.from_public_key_b64,
                                 TransferSignBytes(statement.group_id, t), t.signature_b64);
        !ok) {
      return ok.error();
    }
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

std::optional<GroupRosterStatement> Pick(const std::vector<GroupRosterStatement>& valid) {
  // An owner that handed the group on at epoch e is no longer its owner from e on.
  std::map<std::string, uint64_t> handed_on_at;
  for (const GroupRosterStatement& s : valid) {
    for (const GroupOwnerTransfer& t : s.owner_chain) {
      auto [it, inserted] = handed_on_at.emplace(t.from_identity, t.roster_epoch);
      if (!inserted) {
        it->second = std::min(it->second, t.roster_epoch);
      }
    }
  }
  const GroupRosterStatement* best = nullptr;
  for (const GroupRosterStatement& s : valid) {
    auto it = handed_on_at.find(s.owner_identity);
    if (it != handed_on_at.end() && s.roster_epoch >= it->second) {
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
  json.set("from", transfer.from_identity);
  json.set("from_public_key", transfer.from_public_key_b64);
  json.set("to", transfer.to_identity);
  json.setJsonUInt("roster_epoch", transfer.roster_epoch);
  json.set("signature", transfer.signature_b64);
  return json;
}

Roe<GroupOwnerTransfer> TransferFromJson(const Object& json) {
  GroupOwnerTransfer transfer;
  transfer.from_identity = json.getString("from").value_or("");
  transfer.from_public_key_b64 = json.getString("from_public_key").value_or("");
  transfer.to_identity = json.getString("to").value_or("");
  transfer.roster_epoch = json.getNonNegInt("roster_epoch").value_or(0);
  transfer.signature_b64 = json.getString("signature").value_or("");
  if (transfer.from_identity.empty() || transfer.to_identity.empty() || transfer.signature_b64.empty()) {
    return Error("owner transfer: missing field");
  }
  return transfer;
}

Object ToJson(const GroupRosterStatement& statement) {
  Object json;
  json.set("group_id", statement.group_id);
  json.setJsonUInt("roster_epoch", statement.roster_epoch);
  std::vector<Value> members;
  for (const GroupRosterStatementMember& m : statement.members) {
    Object row;
    row.set("identity", m.identity);
    row.set("role", MemberRoleToString(m.role));
    row.setJsonUInt("joined_at", static_cast<uint64_t>(std::max<int64_t>(m.joined_at, 0)));
    members.push_back(ObjectValue(std::move(row)));
  }
  json.set("members", ArrayValue(std::move(members)));
  json.set("owner", statement.owner_identity);
  json.set("owner_public_key", statement.owner_public_key_b64);
  if (!statement.owner_chain.empty()) {
    std::vector<Value> chain;
    for (const GroupOwnerTransfer& t : statement.owner_chain) {
      chain.push_back(ObjectValue(TransferToJson(t)));
    }
    json.set("owner_chain", ArrayValue(std::move(chain)));
  }
  json.set("owner_signature", statement.owner_signature_b64);
  if (statement.dissolved) {
    json.set("dissolved", true);
  }
  return json;
}

Roe<GroupRosterStatement> FromJson(const Object& json) {
  GroupRosterStatement statement;
  statement.group_id = json.getString("group_id").value_or("");
  statement.roster_epoch = json.getNonNegInt("roster_epoch").value_or(0);
  statement.owner_identity = json.getString("owner").value_or("");
  statement.owner_public_key_b64 = json.getString("owner_public_key").value_or("");
  statement.owner_signature_b64 = json.getString("owner_signature").value_or("");
  statement.dissolved = json.getIf<bool>("dissolved").value_or(false);
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
    m.role = MemberRoleFromString(row->getString("role").value_or("member"));
    m.joined_at = static_cast<int64_t>(row->getNonNegInt("joined_at").value_or(0));
    statement.members.push_back(std::move(m));
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
  return statement;
}

} // namespace group_roster_statement

bool GroupRosterStatement::Lists(const std::string& identity) const {
  return std::any_of(members.begin(), members.end(),
                     [&](const GroupRosterStatementMember& m) { return m.identity == identity; });
}

} // namespace pbr
