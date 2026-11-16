#include "lib/people/AccountLocker.h"

#include "lib/people/ContactJson.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/FileCipher.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/PeerIdForm.h"

#include "common/DeviceName.h"
#include "common/ValueJson.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <tuple>
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
  ByteVector out(kSignDomain, kSignDomain + sizeof(kSignDomain)); // the domain and its terminating zero byte
  out.push_back(kSignVersion);
  AppendLenBytes(out, locker_id);
  AppendU64(out, version);
  AppendLenBytes(out, sealed);
  return out;
}

namespace {

/** One `grants` row of an untrusted copy: every field present and within bounds, else none. */
std::optional<AccountStateGrant> GrantFromJson(const Object& row) {
  AccountStateGrant grant{.grantor_account_id = row.getString("recipient_account_id").value_or(""),
                          .grantee_account_id = row.getString("sender_account_id").value_or(""),
                          .issued_at = row.getIf<int64_t>("issued_at").value_or(0),
                          .expires_at = row.getIf<int64_t>("expires_at").value_or(0),
                          .signature = row.getString("signature").value_or("")};
  const auto fits = [](const std::string& field) {
    return !field.empty() && field.size() <= kMaxLockerGrantFieldBytes;
  };
  if (!fits(grant.grantor_account_id) || !fits(grant.grantee_account_id) || !fits(grant.signature) ||
      grant.issued_at <= 0 || grant.expires_at <= grant.issued_at) {
    return std::nullopt;
  }
  return grant;
}

} // namespace

namespace {

/**
 * The `relays` of an untrusted copy: at most kMaxLockerRelays, each a non-empty origin with a PeerId (MR008), each
 * origin once.
 */
AccountStateRelays RelaysFromJson(const Object& json) {
  AccountStateRelays relays;
  relays.card_issued_at = json.getIf<int64_t>("card_issued_at").value_or(0);
  if (const Array* list = json.getArray("list")) {
    for (const Value& item : list->elements) {
      const Object* row = asObject(item);
      const AccountStateRelay relay{.origin = row ? row->getString("origin").value_or("") : "",
                                    .peer_id = row ? row->getString("peer_id").value_or("") : ""};
      const bool listed = std::any_of(relays.list.begin(), relays.list.end(),
                                      [&relay](const AccountStateRelay& kept) { return kept.origin == relay.origin; });
      if (!relay.origin.empty() && relay.origin.size() <= 256 && IsMlDsaPeerIdForm(relay.peer_id) && !listed &&
          relays.list.size() < kMaxLockerRelays) {
        relays.list.push_back(relay);
      }
    }
  }
  return relays;
}

} // namespace

std::string StateToJson(const AccountLockerState& state) {
  Object root;
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
  std::vector<Value> grants;
  for (const AccountStateGrant& grant : state.extras.grants) {
    Object row;
    row.set("recipient_account_id", grant.grantor_account_id);
    row.set("sender_account_id", grant.grantee_account_id);
    row.set("issued_at", grant.issued_at);
    row.set("expires_at", grant.expires_at);
    row.set("signature", grant.signature);
    grants.push_back(ObjectValue(std::move(row)));
  }
  root.set("grants", ArrayValue(std::move(grants)));
  if (!state.extras.relays.list.empty()) {
    std::vector<Value> list;
    for (const AccountStateRelay& relay : state.extras.relays.list) {
      Object row;
      row.set("origin", relay.origin);
      row.set("peer_id", relay.peer_id);
      list.push_back(ObjectValue(std::move(row)));
    }
    Object relays;
    relays.set("list", ArrayValue(std::move(list)));
    relays.set("card_issued_at", state.extras.relays.card_issued_at);
    root.set("relays", ObjectValue(std::move(relays)));
  }
  std::vector<Value> providers;
  for (const std::string& provider : state.providers) {
    providers.emplace_back(provider);
  }
  root.set("providers", ArrayValue(std::move(providers)));
  root.set("contact_clock", ContactClock::EntriesToJson(state.contact_clock));
  root.set("device_names", DeviceNamesToJson(state.device_names));
  return DumpJson(root);
}

Roe<AccountLockerState> StateFromJson(const std::string& json) {
  auto root = TryParseObject(json);
  if (!root) {
    return Error("account locker: state is not a JSON object");
  }
  AccountLockerState state;
  state.nickname = root->getString("nickname").value_or("");
  if (const Array* contacts = root->getArray("contacts")) {
    for (const Value& item : contacts->elements) {
      if (const Object* row = asObject(item)) {
        state.contacts.push_back(ContactFromJson(*row));
        DropRetiredRelayIds(state.contacts.back()); // a snapshot from before MR001 may list them
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
        AccountCardFloor floor{row->getString("account_id").value_or(""), row->getIf<int64_t>("issued_at").value_or(0)};
        if (!floor.account_id.empty() && floor.issued_at > 0) {
          state.extras.card_floors.push_back(std::move(floor));
        }
      }
    }
  }
  if (const Array* grants = root->getArray("grants")) {
    for (const Value& item : grants->elements) {
      if (const Object* row = asObject(item)) {
        if (auto grant = GrantFromJson(*row)) {
          state.extras.grants.push_back(std::move(*grant));
        }
      }
    }
  }
  if (const Object* relays = root->getObject("relays")) {
    state.extras.relays = RelaysFromJson(*relays);
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
  if (const Array* names = root->getArray("device_names")) {
    state.device_names = DeviceNamesFromJson(*names);
  }
  return state;
}

Value DeviceNamesToJson(const std::vector<DeviceNameEntry>& entries) {
  std::vector<Value> rows;
  for (const DeviceNameEntry& entry : entries) {
    Object row;
    row.set("peer_id", entry.peer_id);
    if (entry.removed) {
      row.set("removed", true);
    } else {
      row.set("name", entry.name);
    }
    row.set("at", entry.at);
    rows.push_back(ObjectValue(std::move(row)));
  }
  return ArrayValue(std::move(rows));
}

std::vector<DeviceNameEntry> DeviceNamesFromJson(const Array& rows) {
  std::vector<DeviceNameEntry> entries;
  for (const Value& item : rows.elements) {
    const Object* row = asObject(item);
    if (!row) {
      continue;
    }
    DeviceNameEntry entry;
    entry.peer_id = row->getString("peer_id").value_or("");
    entry.at = std::clamp<int64_t>(row->getIf<int64_t>("at").value_or(0), 0, kMaxDeviceNameAt);
    entry.removed = row->getIf<bool>("removed").value_or(false);
    if (!entry.removed) {
      entry.name = NormalizeDeviceName(row->getString("name").value_or(""));
    }
    if (!entry.peer_id.empty() && (entry.removed || !entry.name.empty())) {
      entries.push_back(std::move(entry));
    }
  }
  return CapDeviceNames(MergeDeviceNames({}, entries));
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
  std::map<std::string, std::pair<ContactClockEntry, bool>> winners; // account → (entry, from stored)
  for (const ContactClockEntry& entry : ours.contact_clock) {
    winners[entry.account_id] = {entry, false};
  }
  for (const ContactClockEntry& entry : stored.contact_clock) {
    auto found = winners.find(entry.account_id);
    // At a tie the stored register (and its row) stands: a sync with nothing newer writes nothing, whichever
    // device runs it (rows differ per device in their local fields).
    if (found == winners.end() || entry.at >= found->second.first.at) {
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
      merged.contacts.push_back(mine ? *mine : *theirs); // no register on either side (a snapshot before clocks)
      continue;
    }
    if (!winner->second.first.present) {
      continue; // removed, on whichever device was last
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

std::vector<DeviceNameEntry> MergeDeviceNames(const std::vector<DeviceNameEntry>& local,
                                              const std::vector<DeviceNameEntry>& remote) {
  std::map<std::string, DeviceNameEntry> by_peer;
  for (const DeviceNameEntry& entry : local) {
    auto [slot, inserted] = by_peer.try_emplace(entry.peer_id, entry);
    if (!inserted && entry.at > slot->second.at) {
      slot->second = entry;
    }
  }
  for (const DeviceNameEntry& entry : remote) {
    auto [slot, inserted] = by_peer.try_emplace(entry.peer_id, entry);
    if (!inserted && entry.at >= slot->second.at) {
      slot->second = entry; // a tie: the stored copy stands
    }
  }
  std::vector<DeviceNameEntry> merged;
  merged.reserve(by_peer.size());
  for (auto& [peer_id, entry] : by_peer) {
    merged.push_back(std::move(entry));
  }
  return merged;
}

std::vector<DeviceNameEntry> CapDeviceNames(std::vector<DeviceNameEntry> entries) {
  if (entries.size() > kMaxDeviceNameEntries) {
    // Keepers first: live entries before tombstones, the newest first within each, the smaller peer_id on a tie.
    std::sort(entries.begin(), entries.end(), [](const DeviceNameEntry& a, const DeviceNameEntry& b) {
      if (a.removed != b.removed) {
        return !a.removed;
      }
      return a.at != b.at ? a.at > b.at : a.peer_id < b.peer_id;
    });
    entries.resize(kMaxDeviceNameEntries);
  }
  std::sort(entries.begin(), entries.end(),
            [](const DeviceNameEntry& x, const DeviceNameEntry& y) { return x.peer_id < y.peer_id; });
  return entries;
}

std::vector<AccountStateGrant> CapGrants(std::vector<AccountStateGrant> grants, const int64_t now_ms) {
  std::erase_if(grants, [now_ms](const AccountStateGrant& g) { return g.expires_at <= now_ms; });
  // Per grantor the latest issued first (the larger signature on a tie: the same one on every device), then one each.
  std::sort(grants.begin(), grants.end(), [](const AccountStateGrant& a, const AccountStateGrant& b) {
    return std::tie(a.grantor_account_id, b.issued_at, b.signature) <
           std::tie(b.grantor_account_id, a.issued_at, a.signature);
  });
  grants.erase(std::unique(grants.begin(), grants.end(),
                           [](const AccountStateGrant& a, const AccountStateGrant& b) {
                             return a.grantor_account_id == b.grantor_account_id;
                           }),
               grants.end());
  if (grants.size() > kMaxLockerGrants) {
    std::sort(grants.begin(), grants.end(), [](const AccountStateGrant& a, const AccountStateGrant& b) {
      return std::tie(b.issued_at, a.grantor_account_id) < std::tie(a.issued_at, b.grantor_account_id);
    });
    grants.resize(kMaxLockerGrants);
    std::sort(grants.begin(), grants.end(), [](const AccountStateGrant& a, const AccountStateGrant& b) {
      return a.grantor_account_id < b.grantor_account_id;
    });
  }
  return grants;
}

AccountLockerState MergeForPublish(const AccountLockerState& ours, const AccountLockerState& stored) {
  AccountLockerState merged = ours;
  MergeContacts(ours, stored, merged);
  merged.device_names = CapDeviceNames(MergeDeviceNames(ours.device_names, stored.device_names));
  for (const AccountStateGroup& theirs : stored.extras.groups) {
    auto mine = std::find_if(merged.extras.groups.begin(), merged.extras.groups.end(),
                             [&](const AccountStateGroup& g) { return g.group_id == theirs.group_id; });
    if (mine == merged.extras.groups.end()) {
      merged.extras.groups.push_back(theirs);
    } else if (mine->dissolved || theirs.dissolved) {
      // G016: a group the owner ended stays ended, whatever epoch the other copy has.
      if (!mine->dissolved || (theirs.dissolved && theirs.roster_epoch > mine->roster_epoch)) {
        *mine = theirs;
      }
    } else if (theirs.roster_epoch > mine->roster_epoch ||
               (theirs.roster_epoch == mine->roster_epoch && (theirs.left || !mine->left))) {
      // The newer roster; at the same epoch, being out of the group wins, else the stored one stands (members'
      // contact ids are each device's own).
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
  merged.extras.grants.insert(merged.extras.grants.end(), stored.extras.grants.begin(), stored.extras.grants.end());
  if (stored.extras.relays.card_issued_at > merged.extras.relays.card_issued_at) {
    merged.extras.relays = stored.extras.relays; // the list a newer card of ours gave
  }
  for (const std::string& provider : stored.providers) {
    if (std::find(merged.providers.begin(), merged.providers.end(), provider) == merged.providers.end()) {
      merged.providers.push_back(provider);
    }
  }
  // One order whichever device merges, so equal states serialize equal.
  std::sort(merged.extras.groups.begin(), merged.extras.groups.end(),
            [](const AccountStateGroup& a, const AccountStateGroup& b) { return a.group_id < b.group_id; });
  std::sort(merged.extras.card_floors.begin(), merged.extras.card_floors.end(),
            [](const AccountCardFloor& a, const AccountCardFloor& b) { return a.account_id < b.account_id; });
  std::sort(merged.providers.begin(), merged.providers.end());
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
