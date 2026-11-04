#include "common/SchemaVersion.h"

#include "common/ValueJson.h"

#include <limits>
#include <map>
#include "common/PbrCompat.h"

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

} // namespace pbr
