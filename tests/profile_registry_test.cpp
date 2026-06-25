#include "base/data/ProfileRegistry.h"
#include "base/data/SchemaVersion.h"

#include <cassert>
#include <filesystem>
#include <iostream>

int main() {
  const std::string data_dir =
      (std::filesystem::temp_directory_path() / "pp_browser_profile_registry_test").string();
  std::filesystem::remove_all(data_dir);

  auto registry = pbr::ProfileRegistry::Load(data_dir);
  assert(registry);
  assert(registry->ActiveProfileId() == "default");
  assert(std::filesystem::exists(registry->ActiveProfileDataDir()));
  {
    const auto manifest = pbr::SchemaVersion::EnsureProfileManifest(registry->ActiveProfileDataDir());
    assert(manifest);
    assert(std::filesystem::exists(
        std::filesystem::path(registry->ActiveProfileDataDir()) / "manifest.json"));
  }

  std::filesystem::remove_all(data_dir);

  const std::string override_dir =
      (std::filesystem::temp_directory_path() / "pp_browser_profile_override_test").string();
  std::filesystem::remove_all(override_dir);

  registry = pbr::ProfileRegistry::Load(override_dir);
  assert(registry);
  registry->SetSessionProfileOverride("dev");
  assert(registry->EnsureActiveProfile());

  const std::string expected = (std::filesystem::path(override_dir) / "profiles" / "dev").string();
  assert(registry->ActiveProfileDataDir() == expected);
  assert(std::filesystem::exists(expected));

  std::filesystem::remove_all(override_dir);

  std::cout << "profile_registry_test ok\n";
  return 0;
}
