#include "base/data/SchemaVersion.h"

#include "base/data/AtomicFileWrite.h"
#include "common/ValueJson.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <map>

namespace pbr {

namespace {

std::map<int, SchemaVersion::Migrator>& Migrators() {
  static std::map<int, SchemaVersion::Migrator> migrators;
  return migrators;
}

std::optional<int> ReadSchemaVersion(const Object& root) {
  if (auto value = root.getIf<int64_t>("schema_version")) {
    return static_cast<int>(*value);
  }
  if (auto value = root.getNonNegInt("schema_version")) {
    if (*value <= static_cast<uint64_t>(std::numeric_limits<int>::max())) {
      return static_cast<int>(*value);
    }
  }
  return std::nullopt;
}

} // namespace

Roe<void> SchemaVersion::Validate(const Object& root, int expected_version, const std::string& label) {
  auto version = ReadSchemaVersion(root);
  if (!version) {
    return Roe<void>::error(Error("Missing schema_version in " + label));
  }
  if (*version > expected_version) {
    return Roe<void>::error(Error("Unsupported schema version " + std::to_string(*version) + " in " + label +
                                  "; delete the data directory and restart"));
  }
  return {};
}

void SchemaVersion::RegisterMigrator(int from_version, Migrator migrator) {
  Migrators()[from_version] = std::move(migrator);
}

Roe<void> SchemaVersion::RunForwardMigrators(const std::string& path, Object& root, int current_version) {
  int version = ReadSchemaVersion(root).value_or(0);

  while (version < current_version) {
    const auto it = Migrators().find(version);
    if (it == Migrators().end()) {
      return Roe<void>::error(Error("No migrator from schema version " + std::to_string(version) + " for " + path));
    }
    if (auto migrated = it->second(path, root); !migrated) {
      return Roe<void>::error(Error(migrated.error().message));
    }
    version = ReadSchemaVersion(root).value_or(version + 1);
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
    Object root;
    root.set("schema_version", static_cast<int64_t>(kCurrentSchemaVersion));
    if (auto written = AtomicFileWrite::Write(manifest_path.string(), DumpJson(root, 2)); !written) {
      return Roe<void>::error(Error(written.error().message));
    }
    return {};
  }

  std::ifstream in(manifest_path);
  if (!in) {
    return Roe<void>::error(Error("Failed to open profile manifest"));
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto root = TryParseObject(text);
  if (!root) {
    return Roe<void>::error(Error("Failed to parse profile manifest"));
  }

  if (auto validated = Validate(*root, kCurrentSchemaVersion, "manifest.json"); !validated) {
    return Roe<void>::error(Error(validated.error().message));
  }

  if (auto migrated = RunForwardMigrators(manifest_path.string(), *root, kCurrentSchemaVersion); !migrated) {
    return Roe<void>::error(Error(migrated.error().message));
  }

  return {};
}

} // namespace pbr
