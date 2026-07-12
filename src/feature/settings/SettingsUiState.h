#pragma once

#include <string>
#include <vector>

namespace pbr {

struct McpServerUiState {
  std::string id;
  std::string url;
  std::string command;
  std::string args_text;
  bool enabled = true;
};

struct SettingsUiState {
  std::string llm_preset = "cloud";
  std::string llm_base_url;
  std::string llm_model;
  std::string llm_api_key;
  std::string llm_api_key_env;
  std::string promoted_mcp_url;
  std::string search_provider = "duckduckgo";
  std::vector<McpServerUiState> mcp_servers;
  std::string relay_base_url;
  std::string directory_base_url;
  std::string registration_base_url;
  std::string profile_nickname;
  std::string profile_peer_id;
  std::string profile_relay_id;
  std::string profile_public_key;
  std::string profile_registered = "no";
  std::string appearance = "system";
  std::string profile_label;
  std::string config_dir;
  std::string data_dir;
  std::string profile_dir;
  std::string pin_protection_status;
  bool security_can_change_pin = false;
};

struct SettingsSectionListItem {
  std::string id;
  std::string title;
  std::string subtitle;
};

} // namespace pbr
