#include "base/data/ProfileRegistry.h"

#include "base/data/AtomicFileWrite.h"
#include "base/data/SchemaVersion.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

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

  const nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
  if (root.is_discarded()) {
    return Error("Failed to parse profile registry: " + path);
  }

  if (auto checked = SchemaVersion::Validate(root, kSchemaVersion, "profiles.json"); !checked) {
    return checked.error();
  }

  std::string active = "default";
  if (root.contains("active_profile_id") && root["active_profile_id"].is_string()) {
    active = root["active_profile_id"].get<std::string>();
  }

  std::vector<ProfileEntry> profiles;
  if (root.contains("profiles") && root["profiles"].is_array()) {
    for (const auto& item : root["profiles"]) {
      if (!item.is_object()) {
        continue;
      }
      ProfileEntry entry;
      if (item.contains("id") && item["id"].is_string()) {
        entry.id = item["id"].get<std::string>();
      }
      if (item.contains("label") && item["label"].is_string()) {
        entry.label = item["label"].get<std::string>();
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
  nlohmann::json profiles = nlohmann::json::array();
  for (const ProfileEntry& entry : registry.profiles_) {
    profiles.push_back({{"id", entry.id}, {"label", entry.label}});
  }

  const nlohmann::json root = {{"schema_version", kSchemaVersion},
                               {"active_profile_id", registry.active_profile_id_},
                               {"profiles", std::move(profiles)}};

  std::error_code ec;
  std::filesystem::create_directories(data_dir, ec);

  return AtomicFileWrite::Write(RegistryPath(data_dir), root.dump(2));
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
