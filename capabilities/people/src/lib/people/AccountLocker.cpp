#include "lib/people/AccountLocker.h"

#include "lib/people/ContactJson.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/FileCipher.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/crypto/SignedReceive.h"
#include "foundation/crypto/TypedBlob.h"
#include "foundation/error/AppError.h"

#include "common/DeviceName.h"
#include "common/ValueJson.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <tuple>
#include "common/PbrCompat.h"

namespace pbr {
namespace account_locker {
namespace {

constexpr size_t kFirstBucket = 4 * 1024;
constexpr SignedVersions kRecordVersions{.floor = kLockerRecordVersion, .current = kLockerRecordVersion};
constexpr char kStateVersionKey[] = "state_version";
constexpr char kRecordVersionKey[] = "record_version";

void AppendU32(ByteVector& out, uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>(v >> shift));
  }
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

namespace {

ByteVector SignBytesAt(const uint8_t record_version, const ByteVector& locker_id, const uint64_t version,
                       const ByteVector& sealed, const pp::ExtArea& ext) {
  return SignedBytesWriter(kLockerRecordDomain, record_version).Bytes(locker_id).U64(version).Bytes(sealed).Finish(ext);
}

} // namespace

ByteVector SignBytes(const ByteVector& locker_id, const uint64_t version, const ByteVector& sealed,
                     const pp::ExtArea& ext) {
  return SignBytesAt(kLockerRecordVersion, locker_id, version, sealed, ext);
}

Object RecordToJson(const LockerSnapshot& snapshot) {
  Object json;
  json.set(kRecordVersionKey, snapshot.record_version);
  json.set("locker_id", BytesToHex(snapshot.locker_id));
  json.setJsonUInt("version", snapshot.version);
  json.set("signing_public_key_b64", Base64Encode(snapshot.signing_public_key));
  json.set("sealed_b64", Base64Encode(snapshot.sealed));
  WriteExtB64(json, snapshot.ext);
  json.set("signature_b64", Base64Encode(snapshot.signature));
  return json;
}

namespace {

/** Steps 2 and 3 of a record of `record_version` (read before): the fields, the area, the typed blobs' ids. */
Roe<LockerSnapshot> ParseRecord(const Object& json, const int64_t record_version) {
  const std::string locker_id_hex = json.getString("locker_id").value_or("");
  auto locker_id = HexToBytes(locker_id_hex);
  const std::optional<uint64_t> version = json.getNonNegInt("version");
  auto key = Base64Decode(json.getString("signing_public_key_b64").value_or(""));
  auto sealed = Base64Decode(json.getString("sealed_b64").value_or(""));
  auto signature = Base64Decode(json.getString("signature_b64").value_or(""));
  if (!locker_id || locker_id->size() != 32 || BytesToHex(*locker_id) != locker_id_hex || !version || !key || !sealed ||
      !signature) {
    return Error("account locker: a malformed record");
  }
  auto ext = ReadExtB64(json);
  if (!ext) {
    return Error("account locker: a malformed record: " + ext.error().message);
  }
  LockerSnapshot snapshot{.record_version = record_version,
                          .locker_id = std::move(*locker_id),
                          .version = *version,
                          .signing_public_key = std::move(*key),
                          .sealed = std::move(*sealed),
                          .ext = std::move(ext.value()),
                          .signature = std::move(*signature)};
  for (const TypedRead& read :
       {ReadTypedBlob(pp::BlobKind::kSignaturePublicKey, snapshot.signing_public_key, "locker signing key"),
        ReadTypedBlob(pp::BlobKind::kAeadSealed, snapshot.sealed, "locker sealed state"),
        ReadTypedBlob(pp::BlobKind::kSignature, snapshot.signature, "locker signature")}) {
    if (!read.ok()) {
      return read.ToError();
    }
  }
  return snapshot;
}

} // namespace

Roe<LockerSnapshot> RecordFromJson(const Object& json) {
  // 1. The version, before anything else is read.
  const std::optional<int64_t> record_version = OutsideVersionOfJson(json, kRecordVersionKey);
  if (!record_version || *record_version < kLockerRecordVersion) {
    return Error("account locker: a record made by an older app (no record_version 2)");
  }
  if (*record_version > kLockerRecordVersion) {
    return AppError::NewerFormat("account locker: record version " + std::to_string(*record_version) +
                                 ", made by a newer app");
  }
  // 2. The fields and the extension area; 3. the typed blobs' algorithm ids.
  return ParseRecord(json, *record_version);
}

SignedReceipt ReceiveRecord(const Object& json, LockerSnapshot* received) {
  const std::optional<int64_t> record_version = OutsideVersionOfJson(json, kRecordVersionKey);
  LockerSnapshot snapshot;
  TypedRead key;
  TypedRead signature;
  const SignedReceipt receipt = ReceiveSigned({
      .what = "account locker record",
      .outside_version = record_version,
      .versions = kRecordVersions,
      .parse = [&]() -> Roe<void> {
        auto parsed = ParseRecord(json, *record_version);
        if (!parsed) {
          return parsed.error();
        }
        snapshot = std::move(parsed.value());
        return {};
      },
      .algorithms = [&]() -> Roe<void> {
        key = ReadTypedBlob(pp::BlobKind::kSignaturePublicKey, snapshot.signing_public_key, "locker signing key");
        signature = ReadTypedBlob(pp::BlobKind::kSignature, snapshot.signature, "locker signature");
        return !key.ok() ? Roe<void>(key.ToError()) : !signature.ok() ? Roe<void>(signature.ToError()) : Roe<void>{};
      },
      .verify = [&]() -> Roe<void> {
        // The locker id is the hash of the raw key: the key is this locker's, or the record is a forgery.
        if (Blake2b256(key.raw) != snapshot.locker_id) {
          return Error("the signing key is not this locker's");
        }
        return VerifyTyped(key,
                           SignBytesAt(static_cast<uint8_t>(*record_version), snapshot.locker_id, snapshot.version,
                                       snapshot.sealed, snapshot.ext),
                           signature);
      },
      .ext = &snapshot.ext,
  });
  if (receipt.accepted() && received != nullptr) {
    *received = std::move(snapshot);
  }
  return receipt;
}

namespace {

/** `relay:` and 43 base64url characters that decode to 32 bytes, one spelling per id (SIGNED_FORMATS § 3.1, Q5). */
bool IsRelayIdText(const std::string& text) {
  constexpr std::string_view kPrefix = "relay:";
  constexpr size_t kChars = 43;
  if (text.size() != kPrefix.size() + kChars || text.compare(0, kPrefix.size(), kPrefix) != 0) {
    return false;
  }
  const std::string body = text.substr(kPrefix.size());
  auto raw = Base64UrlDecode(body);
  return raw && raw->size() == 32 && Base64UrlEncode(*raw) == body;
}

/**
 * The `relays` of an untrusted copy: at most kMaxLockerRelays, each a non-empty origin with a relay id, each origin
 * once.
 */
AccountStateRelays RelaysFromJson(const Object& json) {
  AccountStateRelays relays;
  relays.card_issued_at = json.getIf<int64_t>("card_issued_at").value_or(0);
  if (const Array* list = json.getArray("list")) {
    for (const Value& item : list->elements) {
      const Object* row = asObject(item);
      const AccountStateRelay relay{.origin = row ? row->getString("origin").value_or("") : "",
                                    .relay_id = row ? row->getString("relay_id").value_or("") : ""};
      const bool listed = std::any_of(relays.list.begin(), relays.list.end(),
                                      [&relay](const AccountStateRelay& kept) { return kept.origin == relay.origin; });
      if (!relay.origin.empty() && relay.origin.size() <= 256 && IsRelayIdText(relay.relay_id) && !listed &&
          relays.list.size() < kMaxLockerRelays) {
        relays.list.push_back(relay);
      }
    }
  }
  return relays;
}

} // namespace

namespace {

/** The fields this build reads (and writes), per row of the state JSON: anything else in a kept row goes back on it. */
constexpr std::string_view kStateFields[] = {"state_version", "nickname", "contacts",  "groups",        "card_floors",
                                             "grants",        "relays",   "providers", "contact_clock", "device_names"};
constexpr std::string_view kContactFields[] = {"id", "local", "remote", "overrides"};
constexpr std::string_view kContactLocalFields[] = {"display_name", "trust"};
constexpr std::string_view kContactRemoteFields[] = {"nickname",   "ids",  "endpoints",    "multiaddrs",
                                                     "fetched_at", "icon", "locator_relay"};
constexpr std::string_view kCardFloorFields[] = {"account_id", "issued_at"};
constexpr std::string_view kGrantFields[] = {
    "grant_version", "recipient_account_id", "sender_account_id", "issued_at", "expires_at", "ext_b64", "signature"};
constexpr std::string_view kRelaysFields[] = {"list", "card_issued_at"};
constexpr std::string_view kRelayFields[] = {"origin", "relay_id"};
constexpr std::string_view kClockFields[] = {"account_id", "at", "present", "digest"};
constexpr std::string_view kDeviceNameFields[] = {"peer_id", "name", "at", "removed"};

/** The rows of `kept[array_key]` by `key_of(row)`: where each written row finds the one it was read as. */
std::map<std::string, const Object*> KeptRows(const Object& kept, const std::string& array_key,
                                              const std::function<std::string(const Object&)>& key_of) {
  std::map<std::string, const Object*> rows;
  if (const Array* items = kept.getArray(array_key)) {
    for (const Value& item : items->elements) {
      if (const Object* row = asObject(item)) {
        rows.emplace(key_of(*row), row);
      }
    }
  }
  return rows;
}

std::string StringField(const Object& row, const std::string& key) {
  return row.getString(key).value_or("");
}

std::string GrantKey(const Object& row) {
  return StringField(row, "recipient_account_id") + "|" + std::to_string(row.getIf<int64_t>("issued_at").value_or(0));
}

/** `row` with the unknown fields of `kept_rows[key]`, if that row exists. */
Value WithKept(Object row, const std::map<std::string, const Object*>& kept_rows, const std::string& key,
               const std::span<const std::string_view> known) {
  if (const auto found = kept_rows.find(key); found != kept_rows.end()) {
    AccountStateCodec::KeepUnknownFields(row, *found->second, known);
  }
  return ObjectValue(std::move(row));
}

/** Each object of the array `rows` with the unknown fields of the kept row of the same `key_field`. */
Value RowsWithKept(const Value& rows, const std::map<std::string, const Object*>& kept_rows,
                   const std::string& key_field, const std::span<const std::string_view> known) {
  std::vector<Value> out;
  if (const Array* items = asArray(rows)) {
    for (const Value& item : items->elements) {
      if (const Object* row = asObject(item)) {
        out.push_back(WithKept(*row, kept_rows, StringField(*row, key_field), known));
      }
    }
  }
  return ArrayValue(std::move(out));
}

/** A contact's row with its kept row's unknown fields, at its top level and in `local` and `remote`. */
Value ContactWithKept(Object row, const std::map<std::string, const Object*>& kept_rows) {
  const auto found = kept_rows.find(StringField(row, "id"));
  if (found == kept_rows.end()) {
    return ObjectValue(std::move(row));
  }
  const Object& kept = *found->second;
  for (const auto& [key, known] :
       {std::pair<std::string, std::span<const std::string_view>>{"local", kContactLocalFields},
        {"remote", kContactRemoteFields}}) {
    const Object* written = row.getObject(key);
    const Object* kept_part = kept.getObject(key);
    if (written != nullptr && kept_part != nullptr) {
      Object merged = *written;
      AccountStateCodec::KeepUnknownFields(merged, *kept_part, known);
      row.set(key, ObjectValue(std::move(merged)));
    }
  }
  AccountStateCodec::KeepUnknownFields(row, kept, kContactFields);
  return ObjectValue(std::move(row));
}

} // namespace

std::string StateToJson(const AccountLockerState& state) {
  const Object& kept = state.kept;
  Object root;
  root.set(kStateVersionKey, kLockerStateVersion);
  root.set("nickname", state.nickname);
  const auto kept_contacts = KeptRows(kept, "contacts", [](const Object& row) { return StringField(row, "id"); });
  std::vector<Value> contacts;
  for (const Contact& contact : state.contacts) {
    contacts.push_back(ContactWithKept(ContactToJson(contact), kept_contacts));
  }
  root.set("contacts", ArrayValue(std::move(contacts)));
  const auto kept_groups = KeptRows(kept, "groups", [](const Object& row) { return StringField(row, "group_id"); });
  std::vector<Value> groups;
  for (const AccountStateGroup& group : state.extras.groups) {
    const auto found = kept_groups.find(group.group_id);
    groups.push_back(
        ObjectValue(AccountStateCodec::GroupToJson(group, found != kept_groups.end() ? found->second : nullptr)));
  }
  root.set("groups", ArrayValue(std::move(groups)));
  const auto kept_floors =
      KeptRows(kept, "card_floors", [](const Object& row) { return StringField(row, "account_id"); });
  std::vector<Value> floors;
  for (const AccountCardFloor& floor : state.extras.card_floors) {
    Object row;
    row.set("account_id", floor.account_id);
    row.set("issued_at", floor.issued_at);
    floors.push_back(WithKept(std::move(row), kept_floors, floor.account_id, kCardFloorFields));
  }
  root.set("card_floors", ArrayValue(std::move(floors)));
  const auto kept_grants = KeptRows(kept, "grants", GrantKey);
  std::vector<Value> grants;
  for (const AccountStateGrant& grant : state.extras.grants) {
    Object row = AccountStateCodec::GrantToJson(grant);
    const std::string key = GrantKey(row);
    grants.push_back(WithKept(std::move(row), kept_grants, key, kGrantFields));
  }
  root.set("grants", ArrayValue(std::move(grants)));
  if (!state.extras.relays.list.empty()) {
    const Object* kept_relays = kept.getObject("relays");
    const auto kept_list =
        kept_relays != nullptr
            ? KeptRows(*kept_relays, "list", [](const Object& row) { return StringField(row, "origin"); })
            : std::map<std::string, const Object*>{};
    std::vector<Value> list;
    for (const AccountStateRelay& relay : state.extras.relays.list) {
      Object row;
      row.set("origin", relay.origin);
      row.set("relay_id", relay.relay_id);
      list.push_back(WithKept(std::move(row), kept_list, relay.origin, kRelayFields));
    }
    Object relays;
    relays.set("list", ArrayValue(std::move(list)));
    relays.set("card_issued_at", state.extras.relays.card_issued_at);
    if (kept_relays != nullptr) {
      AccountStateCodec::KeepUnknownFields(relays, *kept_relays, kRelaysFields);
    }
    root.set("relays", ObjectValue(std::move(relays)));
  }
  std::vector<Value> providers;
  for (const std::string& provider : state.providers) {
    providers.emplace_back(provider);
  }
  root.set("providers", ArrayValue(std::move(providers)));
  const auto kept_clock =
      KeptRows(kept, "contact_clock", [](const Object& row) { return StringField(row, "account_id"); });
  root.set("contact_clock",
           RowsWithKept(ContactClock::EntriesToJson(state.contact_clock), kept_clock, "account_id", kClockFields));
  const auto kept_names = KeptRows(kept, "device_names", [](const Object& row) { return StringField(row, "peer_id"); });
  root.set("device_names",
           RowsWithKept(DeviceNamesToJson(state.device_names), kept_names, "peer_id", kDeviceNameFields));
  AccountStateCodec::KeepUnknownFields(root, kept, kStateFields);
  return DumpJson(root);
}

Roe<AccountLockerState> StateFromJson(const std::string& json) {
  auto root = TryParseObject(json);
  if (!root) {
    return Error("account locker: state is not a JSON object");
  }
  // The state's version first: a newer app's state is not read (nor written over).
  const std::optional<int64_t> state_version = OutsideVersionOfJson(*root, kStateVersionKey);
  if (!state_version || *state_version < kLockerStateVersion) {
    return Error("account locker: a state made by an older app (no state_version 1)");
  }
  if (*state_version > kLockerStateVersion) {
    return AppError::NewerFormat("account locker: state version " + std::to_string(*state_version) +
                                 ", made by a newer app");
  }
  AccountLockerState state;
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
        if (auto grant = AccountStateCodec::GrantFromJson(*row, kMaxLockerGrantFieldBytes)) {
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
  state.kept = std::move(*root);
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
  auto typed_key = TypedSigningPublicKey(keys.signing_public_key);
  if (!typed_key) {
    return typed_key.error();
  }
  LockerSnapshot snapshot;
  snapshot.locker_id = keys.locker_id;
  snapshot.version = version;
  snapshot.signing_public_key = std::move(typed_key.value());
  snapshot.sealed = std::move(*sealed);
  auto signature =
      SignTyped(keys.signing_secret_key, SignBytes(snapshot.locker_id, version, snapshot.sealed, snapshot.ext));
  if (!signature) {
    return signature.error();
  }
  snapshot.signature = std::move(*signature);
  return snapshot;
}

Roe<void> VerifyOwner(const LockerSnapshot& snapshot) {
  return ReceiveRecord(RecordToJson(snapshot), nullptr).ToRoe();
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
  merged.kept = stored.kept; // what the stored copy holds that this build does not read goes back with it (T7)
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

PickedSnapshot PickNewest(const std::vector<LockerSnapshot>& snapshots, const LockerKeys& keys,
                          const uint64_t at_least) {
  PickedSnapshot picked;
  for (const LockerSnapshot& snapshot : snapshots) {
    if (snapshot.version < at_least || (picked.newest && snapshot.version <= picked.newest->version)) {
      continue;
    }
    auto opened = Open(snapshot, keys);
    if (opened) {
      picked.newest = snapshot;
    } else if (AppError::IsNewerFormat(opened.error())) {
      picked.newer_format = true; // a newer app's: not read, and not written over
    }
  }
  return picked;
}

} // namespace account_locker
} // namespace pbr
