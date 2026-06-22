#include "app/Config.h"
#include "app/SessionStore.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
  const std::string config_path =
      (std::filesystem::temp_directory_path() / "pp_browser_session_store_test.json").string();

  pbr::BootstrapResult bootstrap{};
  bootstrap.config = pbr::Config::DefaultAppConfig();
  bootstrap.config_path = config_path;
  bootstrap.data_dir = (std::filesystem::temp_directory_path() / "pp_browser_session_store_data").string();
  bootstrap.profile_data_dir = bootstrap.data_dir + "/profiles/default";
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

  std::filesystem::remove(config_path);
  std::cout << "session_store_test ok\n";
  return 0;
}
