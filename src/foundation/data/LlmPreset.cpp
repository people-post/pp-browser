#include "foundation/data/LlmPreset.h"

#include "foundation/data/PlatformDefaults.h"
#include "foundation/platform/DeploymentProfile.h"
#include "foundation/platform/Platform.h"

#include <cstring>

namespace pbr {

namespace {

constexpr const char* kCloudBaseUrl = "https://api.openai.com/v1";
constexpr const char* kOllamaBaseUrl = "http://localhost:11434/v1";

struct LlmPresetSpec {
  const char* id;
  const char* base_url;
  const char* default_model;
  bool require_api_key;
  bool clears_inline_key;
};

// Brief /v1/chat/completions treats `model` as a provider id (xai, nfscbrief, …),
// not an upstream slug. Upstream model (e.g. grok-4.3) is configured server-side.
constexpr LlmPresetSpec kLlmPresets[] = {
    {"brief", "", "xai", true, true},
    {"cloud", kCloudBaseUrl, "gpt-4o-mini", true, false},
    {"ollama", kOllamaBaseUrl, "llama3.2", false, true},
};

std::string PresetBaseUrl(const LlmPresetSpec& spec) {
  if (std::strcmp(spec.id, "brief") == 0) {
    return BriefLlmBaseUrl();
  }
  return spec.base_url;
}

const LlmPresetSpec* FindPreset(const std::string& preset_id) {
  for (const LlmPresetSpec& spec : kLlmPresets) {
    if (preset_id == spec.id) {
      return &spec;
    }
  }
  return nullptr;
}

bool IsKnownPresetId(const std::string& value) {
  if (value == "custom") {
    return true;
  }
  return FindPreset(value) != nullptr;
}

std::string InferLegacyPreset(const AppConfig& config) {
  if (config.llm.base_url.find("11434") != std::string::npos) {
    return "ollama";
  }
  if (config.llm.base_url == kCloudBaseUrl) {
    return "cloud";
  }
  if (config.llm.base_url == BriefLlmBaseUrl() ||
      config.llm.base_url == PlatformDefaults::For(Platform::Detect()).llm.base_url) {
    return "brief";
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

std::string DefaultModelForPreset(const std::string& preset_id) {
  if (const LlmPresetSpec* spec = FindPreset(preset_id)) {
    return spec->default_model;
  }
  return {};
}

void ApplyPreset(AppConfig& config, const std::string& preset_id, const std::string& custom_base_url) {
  config.llm.preset = preset_id;

  if (const LlmPresetSpec* spec = FindPreset(preset_id)) {
    config.llm.base_url = PresetBaseUrl(*spec);
    config.llm.require_api_key = spec->require_api_key;
    if (spec->clears_inline_key) {
      config.llm.api_key.clear();
      config.llm_api_key_env.clear();
    }
    return;
  }

  config.llm.base_url = custom_base_url;
}

void NormalizeLlmConfig(AppConfig& config) {
  const std::string preset = ResolvePreset(config);
  config.llm.preset = preset;

  if (const LlmPresetSpec* spec = FindPreset(preset)) {
    config.llm.base_url = PresetBaseUrl(*spec);
    config.llm.require_api_key = spec->require_api_key;
    if (spec->clears_inline_key) {
      // Brief / Ollama: do not keep inline keys on disk (Brief key is in identity.enc).
      config.llm.api_key.clear();
      config.llm_api_key_env.clear();
    }
    // Wire model id only. Empty or preset-name-as-model → preset default.
    if (config.llm.model.empty() || IsKnownPresetId(config.llm.model)) {
      config.llm.model = spec->default_model;
    }
  } else if (config.llm.model.empty()) {
    config.llm.model = DefaultModelForPreset("brief");
  }

  ResolveLlmAuthRequirements(config);
}

void ResolveLlmAuthRequirements(AppConfig& config) {
  if (!config.llm.api_key.empty() || !config.llm_api_key_env.empty()) {
    config.llm.require_api_key = true;
    return;
  }

  const std::string preset = ResolvePreset(config);
  if (preset == "brief") {
    config.llm.require_api_key = true;
    return;
  }
  if (preset != "cloud") {
    config.llm.require_api_key = false;
  }
}

} // namespace pbr
