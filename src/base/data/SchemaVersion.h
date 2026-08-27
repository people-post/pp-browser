#pragma once

#include "common/Error.h"
#include "common/Value.h"

#include <functional>
#include <string>

namespace pbr {

class SchemaVersion {
public:
  static constexpr int kCurrentSchemaVersion = 1;

  static Roe<void> Validate(const Object& root, int expected_version, const std::string& label);
  static Roe<void> EnsureProfileManifest(const std::string& profile_data_dir);

  using Migrator = std::function<Roe<void>(const std::string& path, Object& root)>;
  static void RegisterMigrator(int from_version, Migrator migrator);
  static Roe<void> RunForwardMigrators(const std::string& path, Object& root, int current_version);
};

} // namespace pbr
