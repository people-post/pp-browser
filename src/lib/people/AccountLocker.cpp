#include "lib/people/AccountLocker.h"

#include "lib/people/ContactJson.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/FileCipher.h"
#include "foundation/crypto/MlDsa.h"

#include "common/ValueJson.h"

#include <algorithm>
#include <map>
#include <set>
#include "common/PbrCompat.h"

namespace pbr {
namespace account_locker {
namespace {

/** Signed-bytes domain (the relay's convention: UTF-8, then a zero byte, then a version byte). */
constexpr const char kSignDomain[] = "pp-browser:account-locker-v1";
constexpr uint8_t kSignVersion = 1;
constexpr size_t kFirstBucket = 4 * 1024;

void AppendU32(ByteVector& out, uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>(v >> shift));
  }
}

void AppendU64(ByteVector& out, uint64_t v) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>(v >> shift));
  }
}

/** LenBytes: u64 BE length, then the bytes (WIRE_SCHEMAS encoding table). */
void AppendLenBytes(ByteVector& out, const ByteVector& bytes) {
  AppendU64(out, bytes.size());
  out.insert(out.end(), bytes.begin(), bytes.end());
}

/** The AEAD's AAD: the snapshot it belongs to, so a sealed state cannot be replayed under another id or version. */
std::string SealAad(const ByteVector& locker_id, uint64_t version) {
  return "account-locker|" + BytesToHex(locker_id) + "|" + std::to_string(version);
}

size_t BucketFor(size_t length) {
  size_t bucket = kFirstBucket;
  while (bucket < length) {
    bucket *= 4;
  }
  return bucket;
}

} // namespace

Roe<LockerKeys> KeysFrom(const RecoveryKeys& recovery) {
  auto signing = MlDsa::GenerateKeyPairFromSeed(recovery.locker_signing_seed);
  if (!signing) {
    return signing.error();
  }
  LockerKeys keys;
  keys.locker_key = recovery.locker_key;
  keys.signing_public_key = signing->public_key;
  keys.signing_secret_key = signing->secret_key;
  keys.locker_id = Blake2b256(keys.signing_public_key);
  return keys;
}

ByteVector SignBytes(const ByteVector& locker_id, const uint64_t version, const ByteVector& sealed) {
  ByteVector out(kSignDomain, kSignDomain + sizeof(kSignDomain));  // the domain and its terminating zero byte
  out.push_back(kSignVersion);
  AppendLenBytes(out, locker_id);
  AppendU64(out, version);
  AppendLenBytes(out, sealed);
  return out;
}

std::string StateToJson(const AccountLockerState& state) {
  Object root;
  root.set("relay_user_id", state.relay_user_id);
  root.set("nickname", state.nickname);
  std::vector<Value> contacts;
  for (const Contact& contact : state.contacts) {
    contacts.push_back(ObjectValue(ContactToJson(contact)));
  }
  root.set("contacts", ArrayValue(std::move(contacts)));
  std::vector<Value> groups;
  for (const AccountStateGroup& group : state.extras.groups) {
    groups.push_back(ObjectValue(AccountStateCodec::GroupToJson(group)));
  }
  root.set("groups", ArrayValue(std::move(groups)));
  std::vector<Value> floors;
  for (const AccountCardFloor& floor : state.extras.card_floors) {
    Object row;
    row.set("account_id", floor.account_id);
    row.set("issued_at", floor.issued_at);
    floors.push_back(ObjectValue(std::move(row)));
  }
  root.set("card_floors", ArrayValue(std::move(floors)));
  std::vector<Value> providers;
  for (const std::string& provider : state.providers) {
    providers.emplace_back(provider);
  }
  root.set("providers", ArrayValue(std::move(providers)));
  root.set("contact_clock", ContactClock::EntriesToJson(state.contact_clock));
  return DumpJson(root);
}

Roe<AccountLockerState> StateFromJson(const std::string& json) {
  auto root = TryParseObject(json);
  if (!root) {
    return Error("account locker: state is not a JSON object");
  }
  AccountLockerState state;
  state.relay_user_id = root->getString("relay_user_id").value_or("");
  state.nickname = root->getString("nickname").value_or("");
  if (const Array* contacts = root->getArray("contacts")) {
    for (const Value& item : contacts->elements) {
      if (const Object* row = asObject(item)) {
        state.contacts.push_back(ContactFromJson(*row));
      }
    }
  }
  if (const Array* groups = root->getArray("groups")) {
    for (const Value& item : groups->elements) {
      if (const Object* row = asObject(item)) {
        if (auto group = AccountStateCodec::GroupFromJson(*row)) {
          state.extras.groups.push_back(std::move(*group));
        }
      }
    }
  }
  if (const Array* floors = root->getArray("card_floors")) {
    for (const Value& item : floors->elements) {
      if (const Object* row = asObject(item)) {
        AccountCardFloor floor{row->getString("account_id").value_or(""),
                                     row->getIf<int64_t>("issued_at").value_or(0)};
        if (!floor.account_id.empty() && floor.issued_at > 0) {
          state.extras.card_floors.push_back(std::move(floor));
        }
      }
    }
  }
  if (const Array* providers = root->getArray("providers")) {
    for (const Value& item : providers->elements) {
      if (auto url = asString(item)) {
        state.providers.push_back(*url);
      }
    }
  }
  if (const Array* clock = root->getArray("contact_clock")) {
    state.contact_clock = ContactClock::EntriesFromJson(*clock);
  }
  return state;
}

Roe<LockerSnapshot> Seal(const AccountLockerState& state, const LockerKeys& keys, const uint64_t version) {
  const std::string json = StateToJson(state);
  ByteVector padded;
  AppendU32(padded, static_cast<uint32_t>(json.size()));
  padded.insert(padded.end(), json.begin(), json.end());
  padded.resize(BucketFor(padded.size()), 0);
  auto sealed = FileCipher::Encrypt(keys.locker_key, padded, SealAad(keys.locker_id, version));
  if (!sealed) {
    return sealed.error();
  }
  LockerSnapshot snapshot;
  snapshot.locker_id = keys.locker_id;
  snapshot.version = version;
  snapshot.signing_public_key = keys.signing_public_key;
  snapshot.sealed = std::move(*sealed);
  auto signature = MlDsa::Sign(keys.signing_secret_key, SignBytes(snapshot.locker_id, version, snapshot.sealed));
  if (!signature) {
    return signature.error();
  }
  snapshot.signature = std::move(*signature);
  return snapshot;
}

Roe<void> VerifyOwner(const LockerSnapshot& snapshot) {
  if (snapshot.signing_public_key.empty() || Blake2b256(snapshot.signing_public_key) != snapshot.locker_id) {
    return Error("account locker: the signing key is not this locker's");
  }
  auto verified = MlDsa::Verify(snapshot.signing_public_key,
                                SignBytes(snapshot.locker_id, snapshot.version, snapshot.sealed), snapshot.signature);
  if (!verified || !*verified) {
    return Error("account locker: the snapshot's signature does not verify");
  }
  return {};
}

Roe<AccountLockerState> Open(const LockerSnapshot& snapshot, const LockerKeys& keys) {
  if (snapshot.locker_id != keys.locker_id) {
    return Error("account locker: another locker's snapshot");
  }
  if (auto owner = VerifyOwner(snapshot); !owner) {
    return owner.error();
  }
  auto padded = FileCipher::Decrypt(keys.locker_key, snapshot.sealed, SealAad(snapshot.locker_id, snapshot.version));
  if (!padded) {
    return Error("account locker: the snapshot does not open");
  }
  if (padded->size() < 4) {
    return Error("account locker: a truncated snapshot");
  }
  const uint32_t length = (static_cast<uint32_t>((*padded)[0]) << 24) | (static_cast<uint32_t>((*padded)[1]) << 16) |
                          (static_cast<uint32_t>((*padded)[2]) << 8) | static_cast<uint32_t>((*padded)[3]);
  if (length > padded->size() - 4) {
    return Error("account locker: a truncated snapshot");
  }
  return StateFromJson(std::string(padded->begin() + 4, padded->begin() + 4 + length));
}

namespace {

std::string AccountOf(const Contact& contact) {
  for (const mesh::ContactId& id : contact.remote.ids) {
    if (id.kind == mesh::ContactIdKind::Account && !id.value.empty()) {
      return id.value;
    }
  }
  return {};
}

} // namespace

/** Contacts by their clocks: per account the newer register wins (ours on a tie); without one, either side's row. */
void MergeContacts(const AccountLockerState& ours, const AccountLockerState& stored, AccountLockerState& merged) {
  std::map<std::string, std::pair<ContactClockEntry, bool>> winners;  // account → (entry, from stored)
  for (const ContactClockEntry& entry : ours.contact_clock) {
    winners[entry.account_id] = {entry, false};
  }
  for (const ContactClockEntry& entry : stored.contact_clock) {
    auto found = winners.find(entry.account_id);
    if (found == winners.end() || entry.at > found->second.first.at) {
      winners[entry.account_id] = {entry, true};
    }
  }
  const auto row_of = [](const AccountLockerState& side, const std::string& account) -> const Contact* {
    for (const Contact& contact : side.contacts) {
      if (AccountOf(contact) == account) {
        return &contact;
      }
    }
    return nullptr;
  };
  std::set<std::string> accounts;
  for (const Contact& contact : ours.contacts) {
    accounts.insert(AccountOf(contact));
  }
  for (const Contact& contact : stored.contacts) {
    accounts.insert(AccountOf(contact));
  }
  merged.contacts.clear();
  for (const std::string& account : accounts) {
    if (account.empty()) {
      continue;
    }
    const Contact* mine = row_of(ours, account);
    const Contact* theirs = row_of(stored, account);
    auto winner = winners.find(account);
    if (winner == winners.end()) {
      merged.contacts.push_back(mine ? *mine : *theirs);  // no register on either side (a snapshot before clocks)
      continue;
    }
    if (!winner->second.first.present) {
      continue;  // removed, on whichever device was last
    }
    const Contact* row = winner->second.second ? (theirs ? theirs : mine) : (mine ? mine : theirs);
    if (row) {
      merged.contacts.push_back(*row);
    }
  }
  merged.contact_clock.clear();
  for (const auto& [account, entry] : winners) {
    merged.contact_clock.push_back(entry.first);
  }
}

AccountLockerState MergeForPublish(const AccountLockerState& ours, const AccountLockerState& stored) {
  AccountLockerState merged = ours;
  MergeContacts(ours, stored, merged);
  for (const AccountStateGroup& theirs : stored.extras.groups) {
    auto mine = std::find_if(merged.extras.groups.begin(), merged.extras.groups.end(),
                             [&](const AccountStateGroup& g) { return g.group_id == theirs.group_id; });
    if (mine == merged.extras.groups.end()) {
      merged.extras.groups.push_back(theirs);
    } else if (theirs.roster_epoch > mine->roster_epoch) {
      *mine = theirs;
    }
  }
  for (const AccountCardFloor& theirs : stored.extras.card_floors) {
    auto mine = std::find_if(merged.extras.card_floors.begin(), merged.extras.card_floors.end(),
                             [&](const AccountCardFloor& f) { return f.account_id == theirs.account_id; });
    if (mine == merged.extras.card_floors.end()) {
      merged.extras.card_floors.push_back(theirs);
    } else {
      mine->issued_at = std::max(mine->issued_at, theirs.issued_at);
    }
  }
  for (const std::string& provider : stored.providers) {
    if (std::find(merged.providers.begin(), merged.providers.end(), provider) == merged.providers.end()) {
      merged.providers.push_back(provider);
    }
  }
  return merged;
}

std::optional<LockerSnapshot> PickNewest(const std::vector<LockerSnapshot>& snapshots, const LockerKeys& keys,
                                         const uint64_t at_least) {
  std::optional<LockerSnapshot> best;
  for (const LockerSnapshot& snapshot : snapshots) {
    if (snapshot.version < at_least || (best && snapshot.version <= best->version)) {
      continue;
    }
    if (Open(snapshot, keys)) {
      best = snapshot;
    }
  }
  return best;
}

} // namespace account_locker
} // namespace pbr
