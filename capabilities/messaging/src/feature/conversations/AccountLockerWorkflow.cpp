#include "feature/conversations/AccountLockerWorkflow.h"
#include "common/PlatformLimits.h"
#include "common/BoundedRead.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/RecoverySeed.h"
#include "common/AtomicFileWrite.h"
#include "common/DeviceName.h"
#include "common/Utilities.h"
#include "common/ValueJson.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr const char kStateFile[] = "account_locker.json";
/** An exported locker file (M023 §6): the provider record as the relay stores it. */
constexpr const char kLockerFileFormat[] = "pp-browser-account-locker-file-v1";

Roe<LockerKeys> LockerKeysOf(const ByteVector& seed) {
  auto recovery = DeriveRecoveryKeys(seed);
  if (!recovery) {
    return recovery.error();
  }
  return account_locker::KeysFrom(*recovery);
}

Roe<ByteVector> HexToBytes(const std::string& hex) {
  if (hex.size() % 2 != 0) {
    return Error("odd-length hex");
  }
  ByteVector out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    const auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      return -1;
    };
    const int hi = nibble(hex[i]);
    const int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return Error("not lowercase hex");
    }
    out.push_back(static_cast<uint8_t>(hi * 16 + lo));
  }
  return out;
}

} // namespace

LockerRecord LockerRecordFromSnapshot(const LockerSnapshot& snapshot) {
  return LockerRecord{.locker_id = BytesToHex(snapshot.locker_id),
                      .version = snapshot.version,
                      .signing_public_key_b64 = Base64Encode(snapshot.signing_public_key),
                      .sealed_b64 = Base64Encode(snapshot.sealed),
                      .signature_b64 = Base64Encode(snapshot.signature)};
}

Roe<LockerSnapshot> LockerSnapshotFromRecord(const LockerRecord& record) {
  auto id = HexToBytes(record.locker_id);
  auto pk = Base64Decode(record.signing_public_key_b64);
  auto sealed = Base64Decode(record.sealed_b64);
  auto signature = Base64Decode(record.signature_b64);
  if (!id || !pk || !sealed || !signature) {
    return Error("account locker: a malformed record");
  }
  return LockerSnapshot{.locker_id = std::move(*id),
                        .version = record.version,
                        .signing_public_key = std::move(*pk),
                        .sealed = std::move(*sealed),
                        .signature = std::move(*signature)};
}

AccountLockerWorkflow::AccountLockerWorkflow(IdentityStore& identity, ContactsStore& contacts, Sources sources)
    : identity_(identity), contacts_(contacts), sources_(std::move(sources)) {
  redirectLogger("AccountLocker");
}

Roe<std::string> AccountLockerWorkflow::RegisteredAccount() {
  auto identity = identity_.Get();
  if (!identity) {
    return identity.error();
  }
  // The relay keeps a locker for an account registered there (the Account ID signs the write, MR001).
  if (!identity->registered || identity->account_id.empty()) {
    return Error("Not registered with the relay yet");
  }
  return identity->account_id;
}

Roe<AccountLockerState> AccountLockerWorkflow::CurrentState() {
  auto identity = identity_.Get();
  if (!identity) {
    return identity.error();
  }
  AccountLockerState state;
  state.nickname = identity->nickname;
  auto contacts = contacts_.List();
  if (!contacts) {
    return contacts.error();
  }
  state.contacts = std::move(*contacts);
  if (sources_.groups) {
    if (auto groups = sources_.groups()) {
      state.extras.groups = std::move(*groups);
    } else {
      log().warning << "Locker without groups: " << groups.error().message;
    }
  }
  if (sources_.card_floors) {
    if (auto floors = sources_.card_floors()) {
      for (const PeerCardFloor& floor : *floors) {
        state.extras.card_floors.push_back({floor.account_id, floor.issued_at});
      }
    } else {
      log().warning << "Locker without card floors: " << floors.error().message;
    }
  }
  return state;
}

void AccountLockerWorkflow::SetDataDir(std::string data_dir) {
  data_dir_ = std::move(data_dir);
  if (data_dir_.empty()) {
    return;
  }
  std::ifstream in(std::filesystem::path(data_dir_) / kStateFile);
  auto text = ReadBounded(in, kMaxProfileJsonFileBytes);
  if (!text) {
    return;
  }
  auto json = TryParseObject(*text);
  if (!json) {
    return;
  }
  std::lock_guard lock(mutex_);
  if (const Object* seen = json->getObject("highest_version_by_locker")) {
    for (const auto& [locker_id, value] : seen->entries()) {
      if (auto version = asNonNegInt(value)) {
        uint64_t& kept = highest_seen_[locker_id];
        kept = std::max<uint64_t>(kept, *version);
      }
    }
  }
  if (const Array* clock = json->getArray("contact_clock")) {
    clock_.LoadEntries(ContactClock::EntriesFromJson(*clock));
  }
  if (const Array* names = json->getArray("device_names")) {
    device_names_ = account_locker::DeviceNamesFromJson(*names);
  }
}

std::vector<DeviceNameEntry> AccountLockerWorkflow::DeviceNames() const {
  std::lock_guard lock(mutex_);
  return device_names_;
}

bool AccountLockerWorkflow::SetDeviceName(const std::string& peer_id, const std::string& name, const int64_t now_ms) {
  const std::string clean = NormalizeDeviceName(name);
  if (peer_id.empty() || clean.empty()) {
    return false;
  }
  {
    std::lock_guard lock(mutex_);
    auto found = std::find_if(device_names_.begin(), device_names_.end(),
                              [&](const DeviceNameEntry& e) { return e.peer_id == peer_id; });
    if (found != device_names_.end() && (found->removed || found->name == clean)) {
      return false; // a removed device is not renamed; the same name keeps its `at`
    }
    const int64_t at = std::clamp<int64_t>(
        found == device_names_.end() ? now_ms : std::max<int64_t>(now_ms, found->at + 1), 0, kMaxDeviceNameAt);
    device_names_ = account_locker::CapDeviceNames(
        account_locker::MergeDeviceNames(device_names_, {DeviceNameEntry{peer_id, clean, at, false}}));
  }
  Save();
  return true;
}

void AccountLockerWorkflow::ForgetDeviceName(const std::string& peer_id, const int64_t now_ms) {
  if (peer_id.empty()) {
    return;
  }
  {
    std::lock_guard lock(mutex_);
    auto found = std::find_if(device_names_.begin(), device_names_.end(),
                              [&](const DeviceNameEntry& e) { return e.peer_id == peer_id; });
    if (found != device_names_.end() && found->removed) {
      return;
    }
    const int64_t at = std::clamp<int64_t>(
        found == device_names_.end() ? now_ms : std::max<int64_t>(now_ms, found->at + 1), 0, kMaxDeviceNameAt);
    device_names_ = account_locker::CapDeviceNames(
        account_locker::MergeDeviceNames(device_names_, {DeviceNameEntry{peer_id, "", at, true}}));
  }
  Save();
}

void AccountLockerWorkflow::TakeRemoteDeviceNames(const std::vector<DeviceNameEntry>& remote) {
  std::lock_guard lock(mutex_);
  device_names_ = account_locker::CapDeviceNames(account_locker::MergeDeviceNames(device_names_, remote));
}

uint64_t AccountLockerWorkflow::HighestSeen(const std::string& locker_id_hex) const {
  std::lock_guard lock(mutex_);
  auto found = highest_seen_.find(locker_id_hex);
  return found == highest_seen_.end() ? 0 : found->second;
}

void AccountLockerWorkflow::RaiseHighestSeen(const std::string& locker_id_hex, const uint64_t version) {
  {
    std::lock_guard lock(mutex_);
    uint64_t& kept = highest_seen_[locker_id_hex];
    if (version <= kept) {
      return;
    }
    kept = version;
  }
  Save();
}

void AccountLockerWorkflow::Save() {
  // One writer from snapshot to file, so an older snapshot cannot land after a newer one; I/O stays off mutex_.
  std::lock_guard save(save_mutex_);
  Object json;
  {
    std::lock_guard lock(mutex_);
    Object by_locker;
    for (const auto& [id, seen] : highest_seen_) {
      by_locker.setJsonUInt(id, seen);
    }
    json.set("highest_version_by_locker", ObjectValue(std::move(by_locker)));
    json.set("contact_clock", clock_.ToJson());
    json.set("device_names", account_locker::DeviceNamesToJson(device_names_));
  }
  if (data_dir_.empty()) {
    return;
  }
  if (auto written = AtomicFileWrite::Write((std::filesystem::path(data_dir_) / kStateFile).string(), DumpJson(json));
      !written) {
    log().warning << "account locker state not kept: " << written.error().message;
  }
}

Roe<void> AccountLockerWorkflow::TakeRemoteContacts(const std::vector<Contact>& local,
                                                    const AccountLockerState& stored) {
  ContactClock::Plan plan;
  {
    std::lock_guard lock(mutex_);
    plan = clock_.MergeRemote(stored.contact_clock, stored.contacts, local);
  }
  for (const Contact& contact : plan.upsert) {
    if (auto upserted = contacts_.Upsert(contact); !upserted) {
      return upserted.error();
    }
  }
  for (const std::string& contact_id : plan.remove_contact_ids) {
    if (auto removed = contacts_.Remove(contact_id); !removed) {
      return removed.error();
    }
  }
  if (!plan.empty()) {
    log().info << "Contacts from another device: " << plan.upsert.size() << " in, " << plan.remove_contact_ids.size()
               << " out";
  }
  return {};
}

void AccountLockerWorkflow::SetClient(std::shared_ptr<ILockerClient> client) {
  std::lock_guard lock(mutex_);
  client_ = std::move(client);
}

std::shared_ptr<ILockerClient> AccountLockerWorkflow::Client() const {
  std::lock_guard lock(mutex_);
  return client_;
}

Roe<uint64_t> AccountLockerWorkflow::Publish() {
  // One sync at a time: it reads the locker, changes the address book and writes the locker.
  std::lock_guard sync(sync_mutex_);
  const std::shared_ptr<ILockerClient> client = Client(); // this sync's, whatever the provider becomes meanwhile
  if (!client) {
    return Error("No locker provider configured");
  }
  auto seed = identity_.RecoverySeed();
  if (!seed) {
    return seed.error();
  }
  if (!*seed) {
    return Error("This account has no recovery seed (minted before M023): nothing to seal a locker with");
  }
  auto keys = LockerKeysOf(**seed);
  if (!keys) {
    return keys.error();
  }
  auto stored = Fetch(**seed);
  if (!stored) {
    return Error("Locker not read, not published: " + stored.error().message);
  }
  // 1. What changed here since the last sync, then 2. what changed on another device (newer registers win).
  auto local = contacts_.List();
  if (!local) {
    return local.error();
  }
  {
    std::lock_guard lock(mutex_);
    clock_.Observe(*local, util::NowUnixMs());
  }
  if (*stored) {
    if (auto taken = TakeRemoteContacts(*local, (*stored)->state); !taken) {
      return taken.error();
    }
    if (sources_.take_dissolved) {
      std::vector<AccountStateGroup> dissolved;
      for (const AccountStateGroup& group : (*stored)->state.extras.groups) {
        if (group.dissolved) {
          dissolved.push_back(group);
        }
      }
      if (!dissolved.empty()) {
        sources_.take_dissolved(dissolved);
      }
    }
  }
  // 3. Device names: the locker's into ours (the larger `at` wins; a locker without the key erases nothing), then
  // this device's default name, only when no entry for it exists anywhere — never over one it was given.
  if (*stored) {
    TakeRemoteDeviceNames((*stored)->state.device_names);
  }
  if (auto own = sources_.own_devices ? sources_.own_devices() : Roe<OwnDeviceList>(Error("no card source"));
      own && !own->signed_out && !own->self_peer_id.empty()) {
    const std::string system_name = sources_.own_system_name ? NormalizeDeviceName(sources_.own_system_name()) : "";
    if (!system_name.empty()) {
      std::lock_guard lock(mutex_);
      // A tombstone is no entry: a device still on the card that finds itself removed names itself anew, newer.
      const auto own_entry = std::find_if(device_names_.begin(), device_names_.end(),
                                          [&](const DeviceNameEntry& e) { return e.peer_id == own->self_peer_id; });
      if (own_entry == device_names_.end() || own_entry->removed) {
        const int64_t at = std::clamp<int64_t>(own_entry == device_names_.end()
                                                   ? util::NowUnixMs()
                                                   : std::max<int64_t>(util::NowUnixMs(), own_entry->at + 1),
                                               0, kMaxDeviceNameAt);
        device_names_ = account_locker::CapDeviceNames(account_locker::MergeDeviceNames(
            device_names_, {DeviceNameEntry{own->self_peer_id, system_name, at, false}}));
      }
    }
  }
  Save();

  auto state = CurrentState();
  if (!state) {
    return state.error();
  }
  auto registered = RegisteredAccount();
  if (!registered) {
    return registered.error();
  }
  {
    std::lock_guard lock(mutex_);
    state->contact_clock = clock_.Entries();
    state->device_names = device_names_;
  }
  AccountLockerState merged = *stored ? account_locker::MergeForPublish(*state, (*stored)->state) : *state;
  if (!*stored) {
    merged.device_names = account_locker::CapDeviceNames(merged.device_names);
  }
  // Whatever the outcome below, what this publish settled is the table here (an edit made meanwhile survives; an
  // entry the cap cut is not put back by the older table: the cap runs again over the two).
  const auto settle_names = [&]() {
    {
      std::lock_guard lock(mutex_);
      device_names_ =
          account_locker::CapDeviceNames(account_locker::MergeDeviceNames(merged.device_names, device_names_));
    }
    Save();
  };
  if (*stored && account_locker::StateToJson(merged) == account_locker::StateToJson((*stored)->state)) {
    settle_names();
    return (*stored)->version; // nothing new: no write
  }
  const std::string locker_id = BytesToHex(keys->locker_id);
  const uint64_t version = std::max<uint64_t>(static_cast<uint64_t>(util::NowUnixMs()), HighestSeen(locker_id) + 1);
  auto snapshot = account_locker::Seal(merged, *keys, version);
  if (!snapshot) {
    return snapshot.error();
  }
  std::optional<LockerPutRefusal> refused;
  if (auto put = client->Put(*registered, LockerRecordFromSnapshot(*snapshot), &refused); !put) {
    return put.error();
  }
  RaiseHighestSeen(locker_id, version);
  settle_names();
  if (on_written_) {
    on_written_();
  }
  log().info << "Account locker published version=" << version << " contacts=" << merged.contacts.size()
             << " groups=" << merged.extras.groups.size() << " device_names=" << merged.device_names.size()
             << " bytes=" << snapshot->sealed.size();
  return version;
}

Roe<std::optional<AccountLockerWorkflow::Fetched>> AccountLockerWorkflow::Fetch(
    const ByteVector& recovery_seed, const std::vector<std::string>& saved_copies) {
  const std::shared_ptr<ILockerClient> client = Client();
  if (!client && saved_copies.empty()) {
    return Error("No locker provider configured");
  }
  auto keys = LockerKeysOf(recovery_seed);
  if (!keys) {
    return keys.error();
  }
  const std::string locker_id = BytesToHex(keys->locker_id);
  // Every copy within reach — the provider's and any saved file — and the newest valid one wins (M023 §5).
  std::vector<LockerSnapshot> candidates;
  for (const std::string& file_json : saved_copies) {
    auto record = LockerRecordFromFileJson(file_json);
    if (!record) {
      return record.error();
    }
    if (auto snapshot = LockerSnapshotFromRecord(*record)) {
      candidates.push_back(std::move(*snapshot));
    } else {
      return Error("The saved copy is damaged: " + snapshot.error().message);
    }
  }
  if (client) {
    auto record = client->Get(locker_id);
    if (!record && candidates.empty()) {
      return record.error();
    }
    if (!record) {
      log().warning << "Locker provider not read, using the saved copy: " << record.error().message;
    } else if (*record) {
      auto snapshot = LockerSnapshotFromRecord(**record);
      if (!snapshot) {
        return snapshot.error();
      }
      candidates.push_back(std::move(*snapshot));
    }
  }
  if (candidates.empty()) {
    return std::optional<Fetched>{};
  }
  auto newest = account_locker::PickNewest(candidates, *keys, HighestSeen(locker_id));
  if (!newest) {
    return Error(saved_copies.empty()
                     ? "The provider's locker does not verify, or is older than one this device has seen"
                     : "No copy belongs to this recovery phrase, or every copy is older than one this device has seen");
  }
  auto opened = account_locker::Open(*newest, *keys);
  if (!opened) {
    return opened.error();
  }
  RaiseHighestSeen(locker_id, newest->version);
  return std::optional<Fetched>(Fetched{.state = std::move(*opened), .version = newest->version});
}

Roe<std::string> AccountLockerWorkflow::ExportFileJson() {
  std::lock_guard sync(sync_mutex_);
  auto seed = identity_.RecoverySeed();
  if (!seed) {
    return seed.error();
  }
  if (!*seed) {
    return Error("This account has no recovery phrase: nothing to seal a copy with");
  }
  auto keys = LockerKeysOf(**seed);
  if (!keys) {
    return keys.error();
  }
  auto local = contacts_.List();
  if (!local) {
    return local.error();
  }
  auto state = CurrentState();
  if (!state) {
    return state.error();
  }
  auto registered = RegisteredAccount();
  if (!registered) {
    return registered.error();
  }
  {
    std::lock_guard lock(mutex_);
    clock_.Observe(*local, util::NowUnixMs());
    state->contact_clock = clock_.Entries();
  }
  // A version of its own, above any seen: the provider's next write goes higher still, so neither copy hides
  // an edit made after the other.
  const std::string locker_id = BytesToHex(keys->locker_id);
  const uint64_t version = std::max<uint64_t>(static_cast<uint64_t>(util::NowUnixMs()), HighestSeen(locker_id) + 1);
  auto snapshot = account_locker::Seal(*state, *keys, version);
  if (!snapshot) {
    return snapshot.error();
  }
  RaiseHighestSeen(locker_id, version);
  Save();
  Object file;
  file.set("format", kLockerFileFormat);
  file.set("exported_at", util::NowUnixMs());
  file.set("locker", ObjectValue(LockerRecordToJson(LockerRecordFromSnapshot(*snapshot))));
  log().info << "Account locker saved as a file version=" << version << " contacts=" << state->contacts.size();
  return DumpJson(file);
}

Roe<void> CanRenameDevice(const OwnDeviceList& list, const std::string& peer_id) {
  if (list.signed_out) {
    return Error(kSignedOutMessage);
  }
  if (peer_id == list.self_peer_id) {
    return {};
  }
  const bool main = list.devices.empty() || list.devices.front().peer_id == list.self_peer_id;
  if (!main) {
    return Error("Only your main device can rename another device");
  }
  const bool listed = std::any_of(list.devices.begin(), list.devices.end(),
                                  [&](const AccountCardDevice& d) { return d.peer_id == peer_id; });
  if (!listed) {
    return Error("That device is not on your account");
  }
  return {};
}

Roe<LockerRecord> LockerRecordFromFileJson(const std::string& file_json) {
  auto root = TryParseObject(file_json);
  if (!root || root->getString("format").value_or("") != kLockerFileFormat) {
    return Error("This file is not a saved PP account");
  }
  const Object* locker = root->getObject("locker");
  if (!locker) {
    return Error("The saved copy is damaged: no locker in it");
  }
  return LockerRecordFromJson(*locker);
}

} // namespace pbr
