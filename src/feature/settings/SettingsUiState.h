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
  std::string llm_preset = "brief";
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
  /** UI: "on" or "off" — desktop Node participation (N001). */
  std::string node_enabled = "on";
  /** True on desktop so Me → Network shows the help-the-network toggle. */
  bool show_node_toggle = true;
  /** Actual or preferred listen multiaddr (read-only display). */
  std::string libp2p_listen_multiaddr;
  /** Last libp2p start error for Network UX (may be empty). */
  std::string libp2p_status_message;
  std::string reachability_status_label;
  std::string reachability_summary;
  std::string reachability_help_kind;
  bool show_connection_card = false;
  bool show_reachability_help = false;
  std::string circuit_relay_enabled = "off";
  bool show_circuit_relay_toggle = false;
  std::string profile_nickname;
  std::string profile_peer_id;
  std::string profile_relay_id;
  std::string profile_public_key;
  std::string profile_registered = "no";
  std::string profile_registration_status = "not registered";
  std::string profile_registration_expires;
  std::string profile_register_label = "Register on network";
  bool profile_show_register = true;
  bool profile_show_rotate = false;
  /** UI switch value: "auto" or "off". */
  std::string auto_renew_registration = "auto";
  /** UI switch value: "on" or "off" (P005). */
  std::string show_notifications = "on";
  std::string brief_llm_key_masked;
  std::string appearance = "system";
  /** Display label for the theme row / picker value. */
  std::string appearance_label = "System";
  /** Pref: `system` or BCP-47 tag. */
  std::string language = "system";
  /** UI select value: `on` or `off` — disables backdrop frost on compact chrome. */
  std::string reduce_transparency = "off";
  /** Display label for the language row / picker value. */
  std::string language_label = "System";
  std::string profile_label;
  std::string config_dir;
  std::string data_dir;
  std::string profile_dir;
  std::string profile_size_label;
  std::string pin_protection_status;
  bool security_can_change_pin = false;
  /** G007 — everyone | contacts_only | nobody */
  std::string group_invite_policy = "contacts_only";
  /** Display label for the group-invite picker value. */
  std::string group_invite_policy_label = "Contacts only";
  std::string app_name;
  std::string app_version;
};

struct SettingsSectionListItem {
  std::string id;
  std::string title;
  std::string subtitle;
};

} // namespace pbr
