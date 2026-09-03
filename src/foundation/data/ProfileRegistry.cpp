#include "foundation/data/ProfileRegistry.h"

#include "foundation/data/AtomicFileWrite.h"
#include "foundation/data/SchemaVersion.h"
#include "common/ValueJson.h"

#include <filesystem>
#include <fstream>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::string RegistryPath(const std::string& data_dir) {
  return (std::filesystem::path(data_dir) / "profiles.json").string();
}

} // namespace

ProfileRegistry::ProfileRegistry()
    : ProfileRegistry("", "default", {}) {}

ProfileRegistry::ProfileRegistry(std::string data_dir, std::string active_profile_id, std::vector<ProfileEntry> profiles)
    : data_dir_(std::move(data_dir)), active_profile_id_(std::move(active_profile_id)), profiles_(std::move(profiles)) {}

Roe<ProfileRegistry> ProfileRegistry::Load(const std::string& data_dir) {
  const std::string path = RegistryPath(data_dir);
  if (!std::filesystem::exists(path)) {
    ProfileRegistry registry(data_dir, "default", {{"default", "user"}});
    if (auto ensured = registry.EnsureDefaultProfile(); !ensured) {
      return ensured.error();
    }
    if (auto saved = Save(data_dir, registry); !saved) {
      return saved.error();
    }
    return registry;
  }

  std::ifstream in(path);
  if (!in) {
    return Error("Failed to open profile registry: " + path);
  }

  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto root = TryParseObject(text);
  if (!root) {
    return Error("Failed to parse profile registry: " + path);
  }

  if (auto checked = SchemaVersion::Validate(*root, kSchemaVersion, "profiles.json"); !checked) {
    return checked.error();
  }

  std::string active = "default";
  if (auto active_profile_id = root->getString("active_profile_id")) {
    active = *active_profile_id;
  }

  std::vector<ProfileEntry> profiles;
  if (const Array* items = root->getArray("profiles")) {
    for (const Value& item : items->elements) {
      const Object* entry_object = asObject(item);
      if (!entry_object) {
        continue;
      }
      ProfileEntry entry;
      if (auto id = entry_object->getString("id")) {
        entry.id = *id;
      }
      if (auto label = entry_object->getString("label")) {
        entry.label = *label;
      }
      if (!entry.id.empty()) {
        profiles.push_back(std::move(entry));
      }
    }
  }

  ProfileRegistry registry(data_dir, active, profiles);
  if (auto ensured = registry.EnsureDefaultProfile(); !ensured) {
    return ensured.error();
  }
  return registry;
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

  Object root;
  root.set("schema_version", static_cast<int64_t>(kSchemaVersion));
  root.set("active_profile_id", registry.active_profile_id_);
  root.set("profiles", makeArray(std::move(profiles)));

  std::error_code ec;
  std::filesystem::create_directories(data_dir, ec);

  return AtomicFileWrite::Write(RegistryPath(data_dir), DumpJson(root, 2));
}

Roe<void> ProfileRegistry::EnsureActiveProfile() {
  if (auto ensured = EnsureDefaultProfile(); !ensured) {
    return ensured.error();
  }

  const std::string& active = session_override_.empty() ? active_profile_id_ : session_override_;

  bool found = false;
  for (const ProfileEntry& entry : profiles_) {
    if (entry.id == active) {
      found = true;
      break;
    }
  }
  if (!found) {
    profiles_.push_back({active, active});
  }

  const std::string profile_dir = ActiveProfileDataDir();
  std::error_code ec;
  std::filesystem::create_directories(profile_dir, ec);
  return {};
}

Roe<void> ProfileRegistry::EnsureDefaultProfile() {
  if (profiles_.empty()) {
    profiles_.push_back({"default", "user"});
  }

  bool has_default = false;
  for (const ProfileEntry& entry : profiles_) {
    if (entry.id == "default") {
      has_default = true;
      break;
    }
  }
  if (!has_default) {
    profiles_.insert(profiles_.begin(), {"default", "user"});
  }

  if (active_profile_id_.empty()) {
    active_profile_id_ = "default";
  }

  const std::string profile_dir = ActiveProfileDataDir();
  std::error_code ec;
  std::filesystem::create_directories(profile_dir, ec);
  return {};
}

std::string ProfileRegistry::ActiveProfileDataDir() const {
  const std::string& active = session_override_.empty() ? active_profile_id_ : session_override_;
  return (std::filesystem::path(data_dir_) / "profiles" / active).string();
}

} // namespace pbr
