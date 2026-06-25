#pragma once

#include "base/data/Config.h"

#include <string>

namespace pbr {

struct SettingsDraft {
  std::string llm_preset;
  std::string llm_base_url;
  std::string llm_model;
  std::string llm_api_key;
  std::string llm_api_key_env;
};

AppConfig ApplySettingsDraft(const AppConfig& base, const SettingsDraft& draft);

} // namespace pbr
