#include "foundation/data/UserPreferences.h"

#include "foundation/data/AtomicFileWrite.h"
#include "foundation/data/ConfigJson.h"
#include "foundation/data/SchemaVersion.h"
#include "common/ValueJson.h"

#include <filesystem>
#include <fstream>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::string MachinePath(const std::string& data_dir) {
  return (std::filesystem::path(data_dir) / "machine.json").string();
}

std::string ProfilePrefsPath(const std::string& profile_data_dir) {
  return (std::filesystem::path(profile_data_dir) / "preferences.json").string();
}

} // namespace

MachinePreferences UserPreferences::DefaultMachine() {
  return MachinePreferences{};
}

ProfilePreferences UserPreferences::DefaultProfile() {
  return ProfilePreferences{};
}

Roe<MachinePreferences> UserPreferences::LoadMachine(const std::string& data_dir) {
  const std::string path = MachinePath(data_dir);
  if (!std::filesystem::exists(path)) {
    auto prefs = DefaultMachine();
    if (auto saved = SaveMachine(data_dir, prefs); !saved) {
      return saved.error();
    }
    return prefs;
  }

  std::ifstream in(path);
  if (!in) {
    return Error("Failed to open machine preferences: " + path);
  }

  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto root = TryParseObject(text);
  if (!root) {
    return Error("Failed to parse machine preferences: " + path);
  }

  if (auto checked = SchemaVersion::Validate(*root, MachinePreferences::kSchemaVersion, "machine.json");
      !checked) {
    return checked.error();
  }

  MachinePreferences prefs;
  MachinePrefsFromObject(*root, prefs);
  return prefs;
}

Roe<void> UserPreferences::SaveMachine(const std::string& data_dir, const MachinePreferences& prefs) {
  const std::string path = MachinePath(data_dir);
  std::error_code ec;
  std::filesystem::create_directories(data_dir, ec);

  return AtomicFileWrite::Write(path, DumpJson(MachinePrefsToObject(prefs), 2));
}

Roe<ProfilePreferences> UserPreferences::LoadProfile(const std::string& profile_data_dir) {
  const std::string path = ProfilePrefsPath(profile_data_dir);
  if (!std::filesystem::exists(path)) {
    auto prefs = DefaultProfile();
    if (auto saved = SaveProfile(profile_data_dir, prefs); !saved) {
      return saved.error();
    }
    return prefs;
  }

  std::ifstream in(path);
  if (!in) {
    return Error("Failed to open profile preferences: " + path);
  }

  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto root = TryParseObject(text);
  if (!root) {
    return Error("Failed to parse profile preferences: " + path);
  }

  if (auto checked = SchemaVersion::Validate(*root, ProfilePreferences::kSchemaVersion, "preferences.json");
      !checked) {
    return checked.error();
  }

  ProfilePreferences prefs;
  ProfilePrefsFromObject(*root, prefs);
  return prefs;
}

Roe<void> UserPreferences::SaveProfile(const std::string& profile_data_dir, const ProfilePreferences& prefs) {
  const std::string path = ProfilePrefsPath(profile_data_dir);
  std::error_code ec;
  std::filesystem::create_directories(profile_data_dir, ec);

  return AtomicFileWrite::Write(path, DumpJson(ProfilePrefsToObject(prefs), 2));
}

} // namespace pbr
