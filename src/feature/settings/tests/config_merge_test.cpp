#include "base/data/Config.h"
#include "base/data/LlmPreset.h"
#include "feature/settings/SettingsLogic.h"
#include "feature/settings/SettingsUiState.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

TEST(ConfigMergeTest, LoadsDefaultsAndAppliesDrafts) {
  const std::string partial_path = 
      (std::filesystem::temp_directory_path() / "pp_browser_config_merge_test.json").string();
  {
    std::ofstream out(partial_path);
    out << R"json({ "llm": { "model": "custom-model" } })json";
  }

  auto config = pbr::Config::LoadFromFile(partial_path);
  ASSERT_TRUE(static_cast<bool>(config));
  EXPECT_EQ(config->llm.model, "custom-model");
  EXPECT_EQ(config->llm.base_url, "https://www.brief.global/api/llm/v1");
  EXPECT_TRUE(config->llm.require_api_key);
  EXPECT_EQ(config->relay.base_url, "https://www.brief.global/api/relay");

  const pbr::AppConfig defaults = pbr::Config::DefaultAppConfig();
  EXPECT_EQ(defaults.llm.base_url, "https://www.brief.global/api/llm/v1");
  EXPECT_TRUE(defaults.llm.require_api_key);
  EXPECT_EQ(defaults.llm.preset, "brief");
  EXPECT_EQ(defaults.theme, "themes/base.rcss");
  EXPECT_EQ(defaults.relay.base_url, "https://www.brief.global/api/relay");

  std::filesystem::remove(partial_path);

  const std::string roundtrip_path =
      (std::filesystem::temp_directory_path() / "pp_browser_config_roundtrip_test.json").string();
  pbr::AppConfig mutated = defaults;
  mutated.llm.model = "gpt-4o";
  mutated.llm.preset = "cloud";
  mutated.llm.base_url = "https://api.openai.com/v1";
  mutated.llm.require_api_key = true;
  mutated.llm.api_key = "test-key";
  ASSERT_TRUE(pbr::Config::SaveToFile(roundtrip_path, mutated));
  auto reloaded = pbr::Config::LoadFromFile(roundtrip_path);
  ASSERT_TRUE(static_cast<bool>(reloaded));
  EXPECT_EQ(reloaded->llm.model, "gpt-4o");
  EXPECT_EQ(reloaded->llm.preset, "cloud");
  EXPECT_EQ(reloaded->llm.api_key, "test-key");
  std::filesystem::remove(roundtrip_path);

  pbr::AppConfig brief_config = defaults;
  pbr::ApplyPreset(brief_config, "brief", {});
  EXPECT_EQ(brief_config.llm.base_url, "https://www.brief.global/api/llm/v1");
  EXPECT_TRUE(brief_config.llm.require_api_key);
  EXPECT_EQ(brief_config.llm.preset, "brief");

  pbr::AppConfig ollama_config = defaults;
  pbr::ApplyPreset(ollama_config, "ollama", {});
  EXPECT_EQ(ollama_config.llm.base_url, "http://localhost:11434/v1");
  EXPECT_FALSE(ollama_config.llm.require_api_key);
  EXPECT_EQ(ollama_config.llm.preset, "ollama");

  pbr::AppConfig custom_config = defaults;
  custom_config.llm.preset = "custom";
  custom_config.llm.base_url = "https://proxy.example/v1";
  EXPECT_EQ(pbr::ResolvePreset(custom_config), "custom");

  pbr::SettingsDraft draft;
  draft.llm_preset = "brief";
  draft.llm_base_url = "https://www.brief.global/api/llm/v1";
  draft.llm_model = "brief";  // preset-name mistake → normalized to default
  const pbr::AppConfig brief_from_draft = pbr::ApplyLlmSettingsDraft(defaults, draft);
  EXPECT_EQ(brief_from_draft.llm.model, "xai");
  EXPECT_TRUE(brief_from_draft.llm.api_key.empty());

  pbr::SettingsDraft custom_draft;
  custom_draft.llm_preset = "custom";
  custom_draft.llm_base_url = "https://proxy.example/v1";
  custom_draft.llm_model = "my-model";
  custom_draft.llm_api_key_env = "OPENAI_API_KEY";
  const pbr::AppConfig built = pbr::ApplyLlmSettingsDraft(defaults, custom_draft);
  EXPECT_EQ(built.llm.model, "my-model");
  EXPECT_EQ(built.llm.base_url, "https://proxy.example/v1");
  EXPECT_EQ(built.llm_api_key_env, "OPENAI_API_KEY");
  EXPECT_EQ(built.llm.preset, "custom");
  EXPECT_TRUE(built.llm.require_api_key);

  pbr::SettingsDraft ollama_draft;
  ollama_draft.llm_preset = "custom";
  ollama_draft.llm_base_url = "http://192.168.1.10:11434/v1";
  ollama_draft.llm_model = "llama3.2";
  const pbr::AppConfig ollama_built = pbr::ApplyLlmSettingsDraft(defaults, ollama_draft);
  EXPECT_EQ(ollama_built.llm.base_url, "http://192.168.1.10:11434/v1");
  EXPECT_FALSE(ollama_built.llm.require_api_key);
  EXPECT_TRUE(ollama_built.llm.api_key.empty());

  pbr::AppConfig with_key = defaults;
  pbr::ApplyPreset(with_key, "cloud", {});
  with_key.llm.api_key = "saved-key";
  pbr::SettingsDraft model_only;
  model_only.llm_preset = "cloud";
  model_only.llm_base_url = with_key.llm.base_url;
  model_only.llm_model = "new-model";
  const pbr::AppConfig model_only_saved = pbr::ApplyLlmSettingsDraft(with_key, model_only);
  EXPECT_EQ(model_only_saved.llm.model, "new-model");
  EXPECT_EQ(model_only_saved.llm.api_key, "saved-key");

  pbr::SettingsUiState network_state;
  network_state.relay_base_url.clear();
  network_state.directory_base_url.clear();
  network_state.registration_base_url.clear();
  network_state.node_enabled = "off";
  const pbr::AppConfig network = pbr::ApplyNetworkSettingsDraft(defaults, network_state);
  EXPECT_EQ(network.relay.base_url, defaults.relay.base_url);
  EXPECT_EQ(network.directory.base_url, defaults.directory.base_url);
  EXPECT_EQ(network.registration.base_url, defaults.registration.base_url);
  EXPECT_FALSE(network.mesh.node_enabled);
  EXPECT_FALSE(network.mesh.bootstrap_peers.empty());

  pbr::SettingsUiState reset_node = network_state;
  reset_node.node_enabled = "on";
  const pbr::AppConfig after_reset = pbr::ApplyNetworkSettingsDraft(defaults, reset_node);
  EXPECT_TRUE(after_reset.mesh.node_enabled);
}
