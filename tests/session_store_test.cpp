#include "base/data/Config.h"
#include "base/data/SessionStore.h"
#include "base/data/UserPreferences.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
  const std::string config_path =
      (std::filesystem::temp_directory_path() / "pp_browser_session_store_test.json").string();
  const std::string profile_dir =
      (std::filesystem::temp_directory_path() / "pp_browser_session_store_profile").string();

  std::filesystem::create_directories(profile_dir);

  pbr::BootstrapResult bootstrap{};
  bootstrap.config = pbr::Config::DefaultAppConfig();
  bootstrap.config_path = config_path;
  bootstrap.data_dir = (std::filesystem::temp_directory_path() / "pp_browser_session_store_data").string();
  bootstrap.profile_data_dir = profile_dir;
  bootstrap.profile_prefs = pbr::UserPreferences::DefaultProfile();
  bootstrap.profile_prefs.theme = "themes/base.rcss";

  pbr::SessionStore::Instance().Initialize(std::move(bootstrap));

  pbr::AppConfig edited = pbr::SessionStore::Instance().Snapshot().config;
  edited.llm.model = "edited-model";
  edited.llm.preset = "cloud";

  assert(pbr::SessionStore::Instance().SaveConfig(edited));
  assert(pbr::SessionStore::Instance().Snapshot().config.llm.model == "edited-model");

  auto from_disk = pbr::Config::LoadFromFile(config_path);
  assert(from_disk);
  assert(from_disk->llm.model == "edited-model");

  pbr::ProfilePreferences prefs = pbr::SessionStore::Instance().Snapshot().profile_prefs;
  prefs.appearance = "dark";
  assert(pbr::SessionStore::Instance().SaveProfilePrefs(prefs));
  assert(pbr::SessionStore::Instance().Snapshot().profile_prefs.appearance == "dark");

  auto reloaded_prefs = pbr::UserPreferences::LoadProfile(profile_dir);
  assert(reloaded_prefs);
  assert(reloaded_prefs->appearance == "dark");

  assert(pbr::SessionStore::Instance().ReloadProfilePrefs());
  assert(pbr::SessionStore::Instance().Snapshot().profile_prefs.appearance == "dark");

  prefs.appearance = "light";
  assert(pbr::UserPreferences::SaveProfile(profile_dir, prefs));
  assert(pbr::SessionStore::Instance().ReloadProfilePrefs());
  assert(pbr::SessionStore::Instance().Snapshot().profile_prefs.appearance == "light");

  std::filesystem::remove(config_path);
  std::filesystem::remove_all(profile_dir);
  std::cout << "session_store_test ok\n";
  return 0;
}
