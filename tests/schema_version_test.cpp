#include "app/SchemaVersion.h"

#include <iostream>
#include <nlohmann/json.hpp>

namespace {

int fail(const char* message) {
  std::cerr << message << "\n";
  return 1;
}

} // namespace

int main() {
  const nlohmann::json good = {{"schema_version", pbr::SchemaVersion::kCurrentSchemaVersion}};
  const auto good_result =
      pbr::SchemaVersion::Validate(good, pbr::SchemaVersion::kCurrentSchemaVersion, "good.json");
  if (!good_result) {
    std::cerr << "Validate rejected valid manifest: " << good_result.error().message << "\n";
    return 1;
  }

  const nlohmann::json bad = {{"schema_version", 99}};
  const auto bad_result = pbr::SchemaVersion::Validate(bad, 1, "test.json");
  if (!bad_result.isError()) {
    return fail("Validate should reject unsupported schema_version");
  }

  const nlohmann::json missing = nlohmann::json::object();
  const auto missing_result = pbr::SchemaVersion::Validate(missing, 1, "missing.json");
  if (!missing_result.isError()) {
    return fail("Validate should reject missing schema_version");
  }

  std::cout << "schema_version_test ok\n";
  return 0;
}
