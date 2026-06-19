#include "app/UserPreferences.h"

#include "app/SchemaVersion.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace pbr {

namespace {

std::string MachinePath(const std::string& data_dir) {
  return (std::filesystem::path(data_dir) / "machine.json").string();
}

std::string ProfilePrefsPath(const std::string& profile_data_dir) {
  return (std::filesystem::path(profile_data_dir) / "preferences.json").string();
}

MachinePreferences ParseMachine(const nlohmann::json& root) {
  MachinePreferences prefs;
  if (root.contains("schema_version") && root["schema_version"].is_number_integer()) {
    prefs.schema_version = root["schema_version"].get<int>();
  }
  if (root.contains("active_profile_id") && root["active_profile_id"].is_string()) {
    prefs.active_profile_id = root["active_profile_id"].get<std::string>();
  }
  if (root.contains("window") && root["window"].is_object()) {
    const auto& window = root["window"];
    if (window.contains("width") && window["width"].is_number_integer()) {
      prefs.window.width = window["width"].get<int>();
    }
    if (window.contains("height") && window["height"].is_number_integer()) {
      prefs.window.height = window["height"].get<int>();
    }
  }
  if (root.contains("safe_area") && root["safe_area"].is_object()) {
    const auto& safe = root["safe_area"];
    if (safe.contains("top") && safe["top"].is_number_integer()) {
      prefs.safe_area.top = safe["top"].get<int>();
    }
    if (safe.contains("bottom") && safe["bottom"].is_number_integer()) {
      prefs.safe_area.bottom = safe["bottom"].get<int>();
    }
    if (safe.contains("left") && safe["left"].is_number_integer()) {
      prefs.safe_area.left = safe["left"].get<int>();
    }
    if (safe.contains("right") && safe["right"].is_number_integer()) {
      prefs.safe_area.right = safe["right"].get<int>();
    }
  }
  if (root.contains("display") && root["display"].is_object()) {
    const auto& display = root["display"];
    if (display.contains("fullscreen") && display["fullscreen"].is_boolean()) {
      prefs.display.fullscreen = display["fullscreen"].get<bool>();
    }
  }
  return prefs;
}

nlohmann::json MachineToJson(const MachinePreferences& prefs) {
  return {{"schema_version", MachinePreferences::kSchemaVersion},
          {"active_profile_id", prefs.active_profile_id},
          {"window", {{"width", prefs.window.width}, {"height", prefs.window.height}}},
          {"safe_area",
           {{"top", prefs.safe_area.top},
            {"bottom", prefs.safe_area.bottom},
            {"left", prefs.safe_area.left},
            {"right", prefs.safe_area.right}}},
          {"display", {{"fullscreen", prefs.display.fullscreen}}}};
}

ProfilePreferences ParseProfile(const nlohmann::json& root) {
  ProfilePreferences prefs;
  if (root.contains("schema_version") && root["schema_version"].is_number_integer()) {
    prefs.schema_version = root["schema_version"].get<int>();
  }
  if (root.contains("theme") && root["theme"].is_string()) {
    prefs.theme = root["theme"].get<std::string>();
  }
  return prefs;
}

nlohmann::json ProfileToJson(const ProfilePreferences& prefs) {
  return {{"schema_version", ProfilePreferences::kSchemaVersion}, {"theme", prefs.theme}};
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

  const nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
  if (root.is_discarded()) {
    return Error("Failed to parse machine preferences: " + path);
  }

  if (auto checked = SchemaVersion::Validate(root, MachinePreferences::kSchemaVersion, "machine.json"); !checked) {
    return checked.error();
  }

  return ParseMachine(root);
}

Roe<void> UserPreferences::SaveMachine(const std::string& data_dir, const MachinePreferences& prefs) {
  const std::string path = MachinePath(data_dir);
  std::error_code ec;
  std::filesystem::create_directories(data_dir, ec);

  std::ofstream out(path);
  if (!out) {
    return Error("Failed to write machine preferences: " + path);
  }
  out << MachineToJson(prefs).dump(2);
  return {};
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

  const nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
  if (root.is_discarded()) {
    return Error("Failed to parse profile preferences: " + path);
  }

  if (auto checked = SchemaVersion::Validate(root, ProfilePreferences::kSchemaVersion, "preferences.json"); !checked) {
    return checked.error();
  }

  return ParseProfile(root);
}

Roe<void> UserPreferences::SaveProfile(const std::string& profile_data_dir, const ProfilePreferences& prefs) {
  const std::string path = ProfilePrefsPath(profile_data_dir);
  std::error_code ec;
  std::filesystem::create_directories(profile_data_dir, ec);

  std::ofstream out(path);
  if (!out) {
    return Error("Failed to write profile preferences: " + path);
  }
  out << ProfileToJson(prefs).dump(2);
  return {};
}

} // namespace pbr
