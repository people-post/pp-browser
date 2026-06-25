#include "base/data/Config.h"
#include "base/data/LlmPreset.h"
#include "feature/settings/SettingsLogic.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
  const std::string partial_path =
      (std::filesystem::temp_directory_path() / "pp_browser_config_merge_test.json").string();
  {
    std::ofstream out(partial_path);
    out << R"json({ "llm": { "model": "custom-model" } })json";
  }

  auto config = pbr::Config::LoadFromFile(partial_path);
  assert(config);
  assert(config->llm.model == "custom-model");
  assert(config->llm.base_url == "https://api.openai.com/v1");
  assert(config->llm.require_api_key);

  const pbr::AppConfig defaults = pbr::Config::DefaultAppConfig();
  assert(defaults.llm.base_url == "https://api.openai.com/v1");
  assert(defaults.llm.require_api_key);
  assert(defaults.theme == "themes/base.rcss");

  std::filesystem::remove(partial_path);

  const std::string roundtrip_path =
      (std::filesystem::temp_directory_path() / "pp_browser_config_roundtrip_test.json").string();
  pbr::AppConfig mutated = defaults;
  mutated.llm.model = "gpt-4o";
  mutated.llm.preset = "cloud";
  mutated.llm.api_key = "test-key";
  assert(pbr::Config::SaveToFile(roundtrip_path, mutated));
  auto reloaded = pbr::Config::LoadFromFile(roundtrip_path);
  assert(reloaded);
  assert(reloaded->llm.model == "gpt-4o");
  assert(reloaded->llm.preset == "cloud");
  assert(reloaded->llm.api_key == "test-key");
  std::filesystem::remove(roundtrip_path);

  pbr::AppConfig ollama_config = defaults;
  pbr::ApplyPreset(ollama_config, "ollama", {});
  assert(ollama_config.llm.base_url == "http://localhost:11434/v1");
  assert(!ollama_config.llm.require_api_key);
  assert(ollama_config.llm.preset == "ollama");

  pbr::AppConfig custom_config = defaults;
  custom_config.llm.preset = "custom";
  custom_config.llm.base_url = "https://proxy.example/v1";
  assert(pbr::ResolvePreset(custom_config) == "custom");

  pbr::SettingsDraft draft;
  draft.llm_preset = "custom";
  draft.llm_base_url = "https://proxy.example/v1";
  draft.llm_model = "my-model";
  draft.llm_api_key_env = "OPENAI_API_KEY";
  const pbr::AppConfig built = pbr::ApplySettingsDraft(defaults, draft);
  assert(built.llm.model == "my-model");
  assert(built.llm.base_url == "https://proxy.example/v1");
  assert(built.llm_api_key_env == "OPENAI_API_KEY");
  assert(built.llm.preset == "custom");
  assert(built.llm.require_api_key);

  pbr::SettingsDraft ollama_draft;
  ollama_draft.llm_preset = "custom";
  ollama_draft.llm_base_url = "http://192.168.1.10:11434/v1";
  ollama_draft.llm_model = "llama3.2";
  const pbr::AppConfig ollama_built = pbr::ApplySettingsDraft(defaults, ollama_draft);
  assert(ollama_built.llm.base_url == "http://192.168.1.10:11434/v1");
  assert(!ollama_built.llm.require_api_key);
  assert(ollama_built.llm.api_key.empty());

  pbr::AppConfig with_key = defaults;
  with_key.llm.api_key = "saved-key";
  pbr::SettingsDraft model_only;
  model_only.llm_preset = "cloud";
  model_only.llm_base_url = with_key.llm.base_url;
  model_only.llm_model = "new-model";
  const pbr::AppConfig model_only_saved = pbr::ApplySettingsDraft(with_key, model_only);
  assert(model_only_saved.llm.model == "new-model");
  assert(model_only_saved.llm.api_key == "saved-key");

  std::cout << "config_merge_test ok\n";
  return 0;
}
