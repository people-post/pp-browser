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
  /** UI switch value: "on" or "off" (Me → About). Hides ids, endpoints, MCP and paths when off. */
  std::string show_advanced_settings = "off";
  std::string llm_preset = "brief";
  std::string llm_base_url;
  std::string llm_model;
  std::string llm_api_key;
  std::string llm_api_key_env;
  std::string promoted_mcp_url;
  std::string search_provider = "duckduckgo";
  std::vector<McpServerUiState> mcp_servers;
  /** Backend picked in Me -> Network: prod | qa | custom (config `backend.env`). */
  std::string backend_env = "prod";
  /** False when the build has no QA origin: the QA choice is not offered. */
  bool backend_qa_available = false;
  /** The pick differs from the backend this run started with; applies after a restart. */
  bool backend_restart_pending = false;
  std::string relay_base_url;
  std::string directory_base_url;
  std::string registration_base_url;
  /** UI: "on" or "off" — desktop Node participation (N001). */
  std::string node_enabled = "on";
  /** True on desktop so Me → Network shows the help-the-network toggle. */
  bool show_node_toggle = true;
  /** Runtime Amp listen multiaddr (display-only; not persisted). */
  std::string amp_listen_multiaddr;
  /** Last mesh start error for Network UX (may be empty). */
  std::string mesh_status_message;
  std::string reachability_status_label;
  std::string reachability_summary;
  std::string reachability_help_kind;
  bool show_connection_card = false;
  bool show_reachability_help = false;
  std::string circuit_relay_enabled = "off";
  bool show_circuit_relay_toggle = false;
  std::string media_relay_enabled = "on";
  bool show_media_relay_toggle = false;
  std::string dht_enabled = "off";
  bool show_dht_toggle = false;
  std::string prefer_contacts_for_routing = "on";
  /** projects/privacy T1: everyone / contacts / friendly / nobody (mesh.direct_connections). */
  std::string direct_connections = "contacts";
  /** projects/privacy T4: "on" = only org seeds and Friendly contacts' nodes relay for us. */
  std::string trusted_relays_only = "off";
  /** Desktop: stay reachable by contacts with no relay (mesh.reachable_without_relay). */
  std::string reachable_without_relay = "off";
  bool show_prefer_contacts_toggle = false;
  std::string profile_nickname;
  std::string profile_peer_id;
  /** This account's locator, shared with the invite (not shown). */
  std::string profile_contact_locator;
  /** Account id for avatar tone seed; not shown in RML. */
  std::string profile_account_id;
  std::string profile_registered = "no";
  std::string profile_registration_status = "not registered";
  std::string profile_registration_expires;
  std::string profile_register_label = "Register on network";
  bool profile_show_register = true;
  bool profile_show_rotate = false;
  std::string profile_icon_src;
  bool profile_has_icon = false;
  bool profile_icon_uploading = false;
  bool profile_show_clear_icon = false;
  std::string profile_avatar_letter = "?";
  int profile_avatar_tone = 0;
  /** UI switch value: "auto" or "off". */
  std::string auto_renew_registration = "auto";
  /** UI switch value: "on" or "off" (P005). */
  std::string show_notifications = "on";
  /** UI switch value: "on" shows message text in notifications, "off" a neutral line. */
  std::string show_notification_preview = "on";
  /** P011 — stay connected in the background: "on" / "off" (Android only). */
  std::string stay_connected = "off";
  std::string brief_llm_key_masked;
  std::string appearance = "system";
  /** Display label for the theme row / picker value. */
  std::string appearance_label = "System";
  /** Pref: `system` or BCP-47 tag. */
  std::string language = "system";
  /** UI select value: `on` or `off` — disables backdrop frost on compact chrome. */
  std::string reduce_transparency = "off";
  /** UI select value: `on` or `off` — call media diagnostics (V032). */
  std::string call_diagnostics = "off";
  /** UI select value: `on` or `off` — phone Home shows the 动态 widgets section (default off = minimal AI-first). */
  std::string home_show_dynamic = "off";
  std::string home_animated_logo = "on";
  /** UI select value: `on` or `off` — opt-in crash dump upload. */
  std::string crash_reports_enabled = "off";
  /** Display label for the language row / picker value. */
  std::string language_label = "System";
  std::string profile_label;
  std::string config_dir;
  std::string data_dir;
  std::string profile_dir;
  /** all | private | public | cache */
  /** R021 — smart | always_auto | on_demand */
  std::string attachment_download_policy = "smart";
  std::string attachment_download_policy_label;
  /** "on" / "off": with always_auto, download everything on Wi-Fi / on the mobile network. */
  std::string attachment_auto_on_wifi = "on";
  std::string attachment_auto_on_cellular = "off";
  std::string pin_protection_status;
  /** Language-independent flags for pin_protection_status (the text is localized). */
  bool pin_protection_is_default = false;
  bool pin_protection_not_setup = false;
  bool security_can_change_pin = false;
  /** Registered + unlocked — copy a link-device payload. */
  bool security_can_export_link = false;
  /** The account has a recovery phrase to show (M023; accounts minted before have none). */
  bool security_has_recovery_phrase = false;
  /** PIN grace window (#21): 0 / 5 / 15 / 60 minutes, or -1 (always), and its display label. */
  int unlock_grace_minutes = 15;
  std::string unlock_grace_label;
  /** The "Ask for PIN again after" row shows: an own PIN (not the app default) on a platform with a secure store. */
  bool security_grace_shown = false;
  /**
   * Face / fingerprint unlock (at-rest A014): the switch shows (the device has the sensor, an own PIN is set), its
   * state, and its title ("Unlock with Face ID / Touch ID / fingerprint").
   */
  bool security_biometric_shown = false;
  bool security_biometric_on = false;
  std::string security_biometric_label;
  /** G007 — everyone | contacts_only | nobody */
  std::string group_invite_policy = "everyone";
  /** Display label for the group-invite picker value. */
  std::string group_invite_policy_label = "Everyone";
  /** projects/privacy T3 — who may call: everyone | contacts_only | nobody */
  std::string call_invite_policy = "everyone";
  std::string call_invite_policy_label = "Everyone";
  /** Summary of remembered agent tool allow/deny decisions. */
  std::string tool_permissions_summary = "None saved";
  bool tool_permissions_has_saved = false;
  std::string app_name;
  std::string app_version;
};

struct SettingsSectionListItem {
  std::string id;
  std::string title;
  std::string subtitle;
  /** Listed only while Me → About → "Show advanced settings" is on. */
  bool advanced_only = false;
  /** Non-count attention on the section row (e.g. Network reachability nudge). */
  bool attention = false;
};

} // namespace pbr
