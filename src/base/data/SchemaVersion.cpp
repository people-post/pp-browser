#include "app/SchemaVersion.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>

namespace pbr {

namespace {

std::map<int, SchemaVersion::Migrator>& Migrators() {
  static std::map<int, SchemaVersion::Migrator> migrators;
  return migrators;
}

} // namespace

Roe<void> SchemaVersion::Validate(const nlohmann::json& root, int expected_version, const std::string& label) {
  if (!root.contains("schema_version") || !root["schema_version"].is_number_integer()) {
    return Roe<void>::error(Error("Missing schema_version in " + label));
  }
  const int version = root["schema_version"].get<int>();
  if (version > expected_version) {
    return Roe<void>::error(Error("Unsupported schema version " + std::to_string(version) + " in " + label +
                                  "; delete the data directory and restart"));
  }
  return {};
}

void SchemaVersion::RegisterMigrator(int from_version, Migrator migrator) {
  Migrators()[from_version] = std::move(migrator);
}

Roe<void> SchemaVersion::RunForwardMigrators(const std::string& path, nlohmann::json& root, int current_version) {
  int version = 0;
  if (root.contains("schema_version") && root["schema_version"].is_number_integer()) {
    version = root["schema_version"].get<int>();
  }

  while (version < current_version) {
    const auto it = Migrators().find(version);
    if (it == Migrators().end()) {
      return Roe<void>::error(Error("No migrator from schema version " + std::to_string(version) + " for " + path));
    }
    if (auto migrated = it->second(path, root); !migrated) {
      return Roe<void>::error(Error(migrated.error().message));
    }
    version = root.contains("schema_version") && root["schema_version"].is_number_integer()
                  ? root["schema_version"].get<int>()
                  : version + 1;
  }
  return {};
}

Roe<void> SchemaVersion::EnsureProfileManifest(const std::string& profile_data_dir) {
  const std::filesystem::path profile_dir(profile_data_dir);
  const std::filesystem::path manifest_path = profile_dir / "manifest.json";
  std::error_code ec;
  std::filesystem::create_directories(profile_dir, ec);
  if (ec) {
    return Roe<void>::error(Error("Failed to create profile data directory"));
  }

  if (!std::filesystem::exists(manifest_path, ec)) {
    if (ec) {
      return Roe<void>::error(Error("Failed to check profile manifest"));
    }
    const nlohmann::json root = {{"schema_version", kCurrentSchemaVersion}};
    const std::string payload = root.dump(2);
    std::ofstream out(manifest_path, std::ios::trunc);
    if (!out) {
      return Roe<void>::error(Error("Failed to write profile manifest"));
    }
    out << payload;
    out.flush();
    if (!out) {
      return Roe<void>::error(Error("Failed to flush profile manifest"));
    }
    return {};
  }

  std::ifstream in(manifest_path);
  if (!in) {
    return Roe<void>::error(Error("Failed to open profile manifest"));
  }

  nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
  if (root.is_discarded()) {
    return Roe<void>::error(Error("Failed to parse profile manifest"));
  }

  if (auto validated = Validate(root, kCurrentSchemaVersion, "manifest.json"); !validated) {
    return Roe<void>::error(Error(validated.error().message));
  }

  if (auto migrated = RunForwardMigrators(manifest_path.string(), root, kCurrentSchemaVersion);
      !migrated) {
    return Roe<void>::error(Error(migrated.error().message));
  }

  return {};
}

} // namespace pbr
