#include "app/SchemaVersion.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>

int main() {
  const std::string profile_dir =
      (std::filesystem::temp_directory_path() / "pp_browser_schema_manifest_test").string();
  std::filesystem::remove_all(profile_dir);

  auto ensured = pbr::SchemaVersion::EnsureProfileManifest(profile_dir);
  if (!ensured) {
    std::cerr << "EnsureProfileManifest failed: " << ensured.error().message << "\n";
    return 1;
  }

  const std::string manifest_path = (std::filesystem::path(profile_dir) / "manifest.json").string();
  assert(std::filesystem::exists(manifest_path));

  std::ifstream in(manifest_path);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  const nlohmann::json root = nlohmann::json::parse(buffer.str(), nullptr, false);
  assert(!root.is_discarded());
  assert(root["schema_version"].get<int>() == pbr::SchemaVersion::kCurrentSchemaVersion);

  const nlohmann::json bad = {{"schema_version", 99}};
  auto result = pbr::SchemaVersion::Validate(bad, 1, "test.json");
  assert(!result);

  std::filesystem::remove_all(profile_dir);
  std::cout << "schema_version_test ok\n";
  return 0;
}
