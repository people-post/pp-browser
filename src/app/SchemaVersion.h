#pragma once

#include "common/Error.h"

#include <nlohmann/json_fwd.hpp>
#include <functional>
#include <string>

namespace pbr {

class SchemaVersion {
public:
  static constexpr int kCurrentSchemaVersion = 1;

  static Roe<void> Validate(const nlohmann::json& root, int expected_version, const std::string& label);
  static Roe<void> EnsureProfileManifest(const std::string& profile_data_dir);

  using Migrator = std::function<Roe<void>(const std::string& path, nlohmann::json& root)>;
  static void RegisterMigrator(int from_version, Migrator migrator);
  static Roe<void> RunForwardMigrators(const std::string& path, nlohmann::json& root, int current_version);
};

} // namespace pbr
