#include "foundation/data/ProfileRegistry.h"
#include "common/PlatformLimits.h"
#include "common/BoundedRead.h"

#include "common/AtomicFileWrite.h"
#include "common/SchemaVersion.h"
#include "common/ValueJson.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr char kNewProfilePrefix[] = ".new-"; // a profile being made; never a profile id (ids do not start with '.')
constexpr size_t kMaxLabelBytes = 64;

std::string RegistryPath(const std::string& data_dir) {
  return (std::filesystem::path(data_dir) / "profiles.json").string();
}

std::filesystem::path ProfilesRoot(const std::string& data_dir) {
  return std::filesystem::path(data_dir) / "profiles";
}

/** 16 hex digits: not a secret, only unique on this device (a clash is retried). */
std::string RandomProfileId() {
  std::random_device device;
  std::uniform_int_distribution<uint64_t> pick;
  const uint64_t value = pick(device);
  static const char* kHex = "0123456789abcdef";
  std::string id;
  for (int shift = 60; shift >= 0; shift -= 4) {
    id.push_back(kHex[(value >> shift) & 0xF]);
  }
  return id;
}

bool IsValidLabel(const std::string_view label) {
  if (label.empty() || label.size() > kMaxLabelBytes) {
    return false;
  }
  return std::none_of(label.begin(), label.end(), [](const char c) { return static_cast<unsigned char>(c) < 0x20; });
}

} // namespace

bool ProfileRegistry::IsValidProfileId(const std::string_view id) {
  if (id.empty() || id.size() > 64 || id.front() == '.') {
    return false;
  }
  for (const char c : id) {
    const bool ok =
        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

ProfileRegistry::ProfileRegistry() : ProfileRegistry(std::string()) {}

ProfileRegistry::ProfileRegistry(std::string data_dir) : data_dir_(std::move(data_dir)) {}

Roe<ProfileRegistry> ProfileRegistry::Load(const std::string& data_dir) {
  ProfileRegistry registry(data_dir);
  const std::string path = RegistryPath(data_dir);
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    std::ifstream in(path);
    if (!in) {
      return Error("Failed to open profile registry: " + path);
    }
    auto text = ReadBounded(in, kMaxProfileJsonFileBytes);
    if (!text) {
      return Error("Failed to read profile registry: " + text.error().message);
    }
    auto root = TryParseObject(*text);
    if (!root) {
      return Error("Failed to parse profile registry: " + path);
    }
    if (auto checked = SchemaVersion::Validate(*root, kSchemaVersion, "profiles.json"); !checked) {
      return checked.error();
    }
    if (root->getNonNegInt("schema_version").value_or(0) < static_cast<uint64_t>(kSchemaVersion)) {
      // The per-backend registry (schema 1): its profiles are the old format; only its pointer is kept, for pp-node.
      if (auto active = root->getString("active_profile_id"); active && IsValidProfileId(*active)) {
        registry.legacy_active_id_ = *active;
      }
    } else {
      if (const Array* items = root->getArray("profiles")) {
        for (const Value& item : items->elements) {
          const Object* entry_object = asObject(item);
          if (!entry_object) {
            continue;
          }
          ProfileEntry entry;
          entry.id = entry_object->getString("id").value_or(std::string());
          entry.label = entry_object->getString("label").value_or(std::string());
          if (IsValidProfileId(entry.id) && (entry.label.empty() || IsValidLabel(entry.label)) &&
              !registry.EntryOf(entry.id)) {
            registry.profiles_.push_back(std::move(entry));
          }
        }
      }
      if (const Object* active = root->getObject("active")) {
        for (const auto& [backend_id, value] : active->fields()) {
          if (const auto id = asString(value); id && IsValidProfileId(backend_id) && IsValidProfileId(*id)) {
            registry.active_[backend_id] = *id;
          }
        }
      }
    }
  }
  registry.Rescan();
  return registry;
}

void ProfileRegistry::Rescan() {
  manifests_.clear();
  const std::filesystem::path root = ProfilesRoot(data_dir_);
  std::error_code ec;
  if (std::filesystem::is_directory(root, ec)) {
    for (const auto& item : std::filesystem::directory_iterator(root, ec)) {
      std::error_code item_ec;
      if (!item.is_directory(item_ec)) {
        continue;
      }
      const std::string name = item.path().filename().string();
      if (name.rfind(kNewProfilePrefix, 0) == 0) {
        std::filesystem::remove_all(item.path(), item_ec); // a profile whose making stopped half-way: never used
        continue;
      }
      if (IsValidProfileId(name)) {
        manifests_[name] = ReadProfileManifest(item.path().string());
      }
    }
  }
  // An entry for every profile on disk (a label is optional); none for a folder that is gone.
  profiles_.erase(std::remove_if(profiles_.begin(), profiles_.end(),
                                 [this](const ProfileEntry& entry) { return !manifests_.count(entry.id); }),
                  profiles_.end());
  for (const auto& [id, manifest] : manifests_) {
    if (manifest.state == ProfileManifestState::kCurrent && !EntryOf(id)) {
      profiles_.push_back({id, std::string()});
    }
  }
}

Roe<void> ProfileRegistry::Save(const std::string& data_dir, const ProfileRegistry& registry) {
  std::vector<Value> profiles;
  profiles.reserve(registry.profiles_.size());
  for (const ProfileEntry& entry : registry.profiles_) {
    Object item;
    item.set("id", entry.id);
    item.set("label", entry.label);
    profiles.push_back(ObjectValue(std::move(item)));
  }
  Object active;
  for (const auto& [backend_id, id] : registry.active_) {
    active.set(backend_id, id);
  }

  Object root;
  root.set("schema_version", static_cast<int64_t>(kSchemaVersion));
  root.set("profiles", makeArray(std::move(profiles)));
  root.set("active", ObjectValue(std::move(active)));

  std::error_code ec;
  std::filesystem::create_directories(data_dir, ec);
  return AtomicFileWrite::Write(RegistryPath(data_dir), DumpJson(root, 2));
}

std::vector<std::string> ProfileRegistry::OldFormatProfiles() const {
  std::vector<std::string> ids;
  for (const auto& [id, manifest] : manifests_) {
    if (manifest.state == ProfileManifestState::kOldFormat) {
      ids.push_back(id);
    }
  }
  return ids;
}

std::vector<std::string> ProfileRegistry::SkippedProfiles() const {
  std::vector<std::string> ids;
  for (const auto& [id, manifest] : manifests_) {
    if (manifest.state == ProfileManifestState::kMissing || manifest.state == ProfileManifestState::kUnreadable ||
        manifest.state == ProfileManifestState::kNewer) {
      ids.push_back(id);
    }
  }
  return ids;
}

Roe<void> ProfileRegistry::ForgetProfile(const std::string& id) {
  manifests_.erase(id);
  profiles_.erase(
      std::remove_if(profiles_.begin(), profiles_.end(), [&id](const ProfileEntry& entry) { return entry.id == id; }),
      profiles_.end());
  for (auto it = active_.begin(); it != active_.end();) {
    it = it->second == id ? active_.erase(it) : std::next(it);
  }
  if (active_profile_id_ == id) {
    active_profile_id_.clear();
  }
  return Save(data_dir_, *this);
}

Roe<void> ProfileRegistry::ConvertOldFormatProfilesToNode() {
  const std::vector<std::string> old = OldFormatProfiles();
  if (old.empty()) {
    return {};
  }
  const std::string node(profile_network::kNode);
  // The node's home: the old registry's active profile, else `default`; the others keep their old id as a label.
  std::string home;
  for (const std::string& candidate : {legacy_active_id_, std::string("default")}) {
    if (!candidate.empty() && std::find(old.begin(), old.end(), candidate) != old.end()) {
      home = candidate;
      break;
    }
  }
  if (active_.count(node)) {
    home.clear(); // a node already has its home profile
  }
  for (const std::string& id : old) {
    const std::string dir = ProfileDataDir(id);
    if (auto converted = ConvertOldProfileManifestToNode(dir); !converted) {
      return converted.error();
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dir) / "backends" / node, ec);
    if (ec) {
      return Error("Failed to create " + dir + "/backends/" + node + ": " + ec.message());
    }
    manifests_[id] = ReadProfileManifest(dir);
    const std::string label = id == home ? std::string() : id;
    if (ProfileEntry* entry = EntryOf(id)) {
      entry->label = label;
    } else {
      profiles_.push_back({id, label});
    }
  }
  if (!home.empty()) {
    active_[node] = home;
  }
  return Save(data_dir_, *this);
}

ProfileManifestRead ProfileRegistry::ManifestOf(const std::string& id) const {
  const auto it = manifests_.find(id);
  return it == manifests_.end() ? ProfileManifestRead{} : it->second;
}

bool ProfileRegistry::HasBackend(const std::string& id, const std::string& backend_id) const {
  std::error_code ec;
  return std::filesystem::is_directory(std::filesystem::path(ProfileDataDir(id)) / "backends" / backend_id, ec);
}

bool ProfileRegistry::IsHomeOf(const std::string& id, const std::string& backend_id,
                               const std::string_view network) const {
  const ProfileManifestRead manifest = ManifestOf(id);
  return manifest.state == ProfileManifestState::kCurrent && manifest.network == network && HasBackend(id, backend_id);
}

ProfileEntry* ProfileRegistry::EntryOf(const std::string& id) {
  for (ProfileEntry& entry : profiles_) {
    if (entry.id == id) {
      return &entry;
    }
  }
  return nullptr;
}

Roe<std::string> ProfileRegistry::CreateProfile(const std::string_view network, const std::string& backend_id,
                                                const std::string& label) {
  const std::filesystem::path root = ProfilesRoot(data_dir_);
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  if (ec) {
    return Error("Failed to create " + root.string() + ": " + ec.message());
  }
  for (int attempt = 0; attempt < 8; ++attempt) {
    const std::string id = RandomProfileId();
    const std::filesystem::path target = root / id;
    if (std::filesystem::exists(target, ec)) {
      continue;
    }
    // Made whole in a hidden folder — manifest first, then its backend — and renamed into place, so no profile is ever
    // seen without its network or its home backend (PM003).
    const std::filesystem::path staging = root / (kNewProfilePrefix + id);
    std::filesystem::remove_all(staging, ec);
    if (auto written = WriteNewProfileManifest(staging.string(), network); !written) {
      return written.error();
    }
    std::filesystem::create_directories(staging / "backends" / backend_id, ec);
    if (ec) {
      std::filesystem::remove_all(staging, ec);
      return Error("Failed to create a profile's backend folder: " + ec.message());
    }
    std::filesystem::rename(staging, target, ec);
    if (ec) {
      std::error_code cleanup;
      std::filesystem::remove_all(staging, cleanup);
      return Error("Failed to create profile " + id + ": " + ec.message());
    }
    manifests_[id] = ReadProfileManifest(target.string());
    profiles_.push_back({id, label});
    return id;
  }
  return Error("Failed to pick a new profile id");
}

void ProfileRegistry::Activate(const std::string& id, const std::string& backend_id, const std::string_view network) {
  active_profile_id_ = id;
  active_backend_id_ = backend_id;
  active_network_ = std::string(network);
}

Roe<void> ProfileRegistry::SelectProfileForBackend(const std::string& backend_id, const std::string_view network) {
  if (!IsValidProfileId(backend_id)) {
    return Error("invalid backend id: " + backend_id);
  }
  if (!IsKnownProfileNetwork(network)) {
    return Error("unknown profile network: " + std::string(network));
  }
  if (const auto pointer = active_.find(backend_id); pointer != active_.end()) {
    const std::string& id = pointer->second;
    if (IsHomeOf(id, backend_id, network)) {
      Activate(id, backend_id, network);
      return {};
    }
    const ProfileManifestState state = ManifestOf(id).state;
    if (state == ProfileManifestState::kUnreadable || state == ProfileManifestState::kNewer) {
      return Error("profile " + id + " of backend " + backend_id + " cannot be opened: its manifest is " +
                   (state == ProfileManifestState::kNewer ? "from a newer version of the app" : "unreadable"));
    }
    // Gone, or not this backend's home: the pointer is stale.
  }

  // A labelled profile is `--profile`'s (dev isolation): never a backend's own.
  std::string target;
  for (const ProfileEntry& entry : profiles_) {
    if (entry.label.empty() && IsHomeOf(entry.id, backend_id, network)) {
      target = entry.id;
      break;
    }
  }
  if (target.empty()) {
    auto created = CreateProfile(network, backend_id, std::string());
    if (!created) {
      return created.error();
    }
    target = *created;
  }
  active_[backend_id] = target;
  Activate(target, backend_id, network);
  return Save(data_dir_, *this);
}

Roe<void> ProfileRegistry::SelectProfileByLabel(const std::string& label, const std::string& backend_id,
                                                const std::string_view network) {
  if (!IsValidLabel(label)) {
    return Error("invalid profile label (1–64 characters, no control characters): " + label);
  }
  if (!IsValidProfileId(backend_id)) {
    return Error("invalid backend id: " + backend_id);
  }
  if (!IsKnownProfileNetwork(network)) {
    return Error("unknown profile network: " + std::string(network));
  }
  for (const ProfileEntry& entry : profiles_) {
    if (entry.label == label && IsHomeOf(entry.id, backend_id, network)) {
      Activate(entry.id, backend_id, network);
      return {};
    }
  }
  auto created = CreateProfile(network, backend_id, label);
  if (!created) {
    return created.error();
  }
  Activate(*created, backend_id, network);
  return Save(data_dir_, *this);
}

Roe<void> ProfileRegistry::ReplaceActiveProfile() {
  if (active_profile_id_.empty() || active_backend_id_.empty()) {
    return Error("no active profile to replace");
  }
  const std::string old_id = active_profile_id_;
  std::string label;
  if (const ProfileEntry* entry = EntryOf(old_id)) {
    label = entry->label;
  }
  auto created = CreateProfile(active_network_, active_backend_id_, label);
  if (!created) {
    return created.error();
  }
  for (auto& [backend_id, id] : active_) {
    if (id == old_id) {
      id = *created;
    }
  }
  manifests_.erase(old_id);
  profiles_.erase(std::remove_if(profiles_.begin(), profiles_.end(),
                                 [&old_id](const ProfileEntry& entry) { return entry.id == old_id; }),
                  profiles_.end());
  active_profile_id_ = *created;
  return Save(data_dir_, *this);
}

std::string ProfileRegistry::ProfileDataDir(const std::string& id) const {
  return (ProfilesRoot(data_dir_) / id).string();
}

std::string ProfileRegistry::ActiveProfileDataDir() const {
  return active_profile_id_.empty() ? std::string() : ProfileDataDir(active_profile_id_);
}

} // namespace pbr
