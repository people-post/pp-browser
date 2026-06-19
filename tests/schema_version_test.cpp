#include "app/SchemaVersion.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

int fail(const char* message) {
  std::cerr << message << "\n";
  return 1;
}

std::filesystem::path uniqueTempProfileDir() {
  const auto suffix =
#if defined(_WIN32)
      std::to_string(_getpid());
#else
      std::to_string(getpid());
#endif
  return std::filesystem::temp_directory_path() / ("pp_browser_schema_manifest_test_" + suffix);
}

} // namespace

int main() {
  const std::filesystem::path profile_dir = uniqueTempProfileDir();
  std::filesystem::remove_all(profile_dir);

  auto ensured = pbr::SchemaVersion::EnsureProfileManifest(profile_dir.string());
  if (!ensured) {
    std::cerr << "EnsureProfileManifest failed: " << ensured.error().message << "\n";
    return 1;
  }

  const std::filesystem::path manifest_path = profile_dir / "manifest.json";
  if (!std::filesystem::exists(manifest_path)) {
    return fail("manifest.json was not created");
  }

  std::ifstream in(manifest_path);
  if (!in) {
    return fail("failed to open manifest.json");
  }

  std::ostringstream buffer;
  buffer << in.rdbuf();
  const nlohmann::json root = nlohmann::json::parse(buffer.str(), nullptr, false);
  if (root.is_discarded()) {
    return fail("failed to parse manifest.json");
  }
  if (!root.contains("schema_version") || !root["schema_version"].is_number_integer()) {
    return fail("manifest.json missing integer schema_version");
  }
  if (root["schema_version"].get<int>() != pbr::SchemaVersion::kCurrentSchemaVersion) {
    return fail("manifest.json schema_version mismatch");
  }

  const nlohmann::json bad = {{"schema_version", 99}};
  const auto result = pbr::SchemaVersion::Validate(bad, 1, "test.json");
  if (!result.isError()) {
    return fail("Validate should reject unsupported schema_version");
  }

  std::filesystem::remove_all(profile_dir);
  std::cout << "schema_version_test ok\n";
  return 0;
}
