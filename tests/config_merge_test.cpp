#include "app/Config.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
  const std::string path = (std::filesystem::temp_directory_path() / "pp_browser_config_merge_test.json").string();
  {
    std::ofstream out(path);
    out << R"json({ "llm": { "model": "custom-model" } })json";
  }

  auto config = pbr::Config::LoadFromFile(path);
  assert(config);
  assert(config->llm.model == "custom-model");
  assert(config->llm.base_url == "https://api.openai.com/v1");
  assert(config->llm.require_api_key);

  const pbr::AppConfig defaults = pbr::Config::DefaultAppConfig();
  assert(defaults.llm.base_url == "https://api.openai.com/v1");
  assert(defaults.llm.require_api_key);
  assert(defaults.theme == "themes/base.rcss");

  std::filesystem::remove(path);
  std::cout << "config_merge_test ok\n";
  return 0;
}
