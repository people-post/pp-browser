#include "feature/settings/SettingsLogic.h"

#include "base/data/LlmPreset.h"

namespace pbr {

AppConfig ApplySettingsDraft(const AppConfig& base, const SettingsDraft& draft) {
  AppConfig config = base;

  ApplyPreset(config, draft.llm_preset, draft.llm_base_url);
  config.llm.model = draft.llm_model;

  if (!draft.llm_api_key.empty()) {
    config.llm.api_key = draft.llm_api_key;
    config.llm_api_key_env.clear();
  } else if (!draft.llm_api_key_env.empty()) {
    config.llm_api_key_env = draft.llm_api_key_env;
    config.llm.api_key.clear();
  } else if (ResolvePreset(config) != "cloud") {
    config.llm.api_key.clear();
    config.llm_api_key_env.clear();
  }

  ResolveLlmAuthRequirements(config);
  return config;
}

} // namespace pbr
