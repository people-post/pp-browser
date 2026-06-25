#pragma once

#include <string>

namespace pbr {

struct SettingsUiState {
  std::string llm_preset = "cloud";
  std::string llm_base_url;
  std::string llm_model;
  std::string llm_api_key;
  std::string llm_api_key_env;
  std::string appearance = "system";
  std::string profile_label;
  std::string config_dir;
  std::string data_dir;
  std::string profile_dir;
};

struct SettingsSectionListItem {
  std::string id;
  std::string title;
  std::string subtitle;
};

} // namespace pbr
