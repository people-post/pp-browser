#include "base/data/LlmPreset.h"

#include "base/platform/Platform.h"
#include "base/platform/PlatformDefaults.h"

namespace pbr {

namespace {

constexpr const char* kOllamaBaseUrl = "http://localhost:11434/v1";

struct LlmPresetSpec {
  const char* id;
  const char* base_url;
  bool require_api_key;
  bool clears_inline_key;
};

constexpr LlmPresetSpec kLlmPresets[] = {
    {"cloud", "https://api.openai.com/v1", true, false},
    {"ollama", kOllamaBaseUrl, false, true},
};

const LlmPresetSpec* FindPreset(const std::string& preset_id) {
  for (const LlmPresetSpec& spec : kLlmPresets) {
    if (preset_id == spec.id) {
      return &spec;
    }
  }
  return nullptr;
}

std::string InferLegacyPreset(const AppConfig& config) {
  if (config.llm.base_url.find("11434") != std::string::npos) {
    return "ollama";
  }
  if (config.llm.base_url == PlatformDefaults::For(Platform::Detect()).llm.base_url) {
    return "cloud";
  }
  return "custom";
}

} // namespace

std::string ResolvePreset(const AppConfig& config) {
  if (!config.llm.preset.empty()) {
    return config.llm.preset;
  }
  return InferLegacyPreset(config);
}

void ApplyPreset(AppConfig& config, const std::string& preset_id, const std::string& custom_base_url) {
  config.llm.preset = preset_id;

  if (const LlmPresetSpec* spec = FindPreset(preset_id)) {
    config.llm.base_url = spec->base_url;
    config.llm.require_api_key = spec->require_api_key;
    if (spec->clears_inline_key) {
      config.llm.api_key.clear();
      config.llm_api_key_env.clear();
    }
    return;
  }

  config.llm.base_url = custom_base_url;
}

void ResolveLlmAuthRequirements(AppConfig& config) {
  if (!config.llm.api_key.empty() || !config.llm_api_key_env.empty()) {
    config.llm.require_api_key = true;
    return;
  }

  if (ResolvePreset(config) != "cloud") {
    config.llm.require_api_key = false;
  }
}

} // namespace pbr
