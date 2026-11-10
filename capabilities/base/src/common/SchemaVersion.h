#pragma once

#include "common/Error.h"
#include "common/Value.h"

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** A JSON file's `schema_version`: refuse a newer one, migrate an older one forward. */
class SchemaVersion {
public:
  static constexpr int kCurrentSchemaVersion = 1;

  static pp::Roe<void> Validate(const pp::common::Object& root, int expected_version, const std::string& label);

  using Migrator = std::function<pp::Roe<void>(const std::string& path, pp::common::Object& root)>;
  static void RegisterMigrator(int from_version, Migrator migrator);
  static pp::Roe<void> RunForwardMigrators(const std::string& path, pp::common::Object& root, int current_version);
};

} // namespace pbr
