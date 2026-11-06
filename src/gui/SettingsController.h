#pragma once

#include "foundation/data/SessionStore.h"
#include "feature/settings/SettingsCommands.h"
#include "feature/settings/SettingsSectionHandler.h"
#include "feature/settings/SettingsSections.h"
#include "feature/settings/SettingsUiState.h"
#include "gui/SettingsCasLibrary.h"
#include "gui/SettingsChoicePage.h"
#include "gui/SettingsHost.h"
#include "gui/SettingsProfileActions.h"
#include "gui/SettingsOwnDevices.h"
#include "gui/SettingsSecurityActions.h"
#include "gui/StorageManagerController.h"
#include "gui/shell/ShellFeedbackPorts.h"
#include "foundation/platform/PlatformShare.h"
#include "gui/shell/ShellNavigationPorts.h"
#include "gui/UnlockEnsurePorts.h"
#include "common/Error.h"
#include "common/Module.h"

#include <ui/data/DataModelHandle.h>
#include <ui/dom/Event.h>
#include <ui/base/Types.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "common/PbrCompat.h"

namespace ui {
class Context;
}

namespace pbr {

class SettingsController : public Module, public SettingsHost {
public:
  struct SectionListRow {
    ui::String id;
    ui::String title;
    ui::String subtitle;
    bool attention = false;
  };

  struct McpServerRow {
    ui::String id;
    ui::String url;
    ui::String command;
    ui::String args_text;
    bool enabled = true;
  };


  SettingsController();
  ~SettingsController() override = default;

  /** App fills ports (session, messaging views, register, UPnP, …). Not a process singleton. */
  void BindCommands(SettingsCommands commands);
  /**
   * A "?" beside a hard setting: phones ask the AI in the ask-AI panel (a one-off question; the panel offers PP 客服
   * too), the desktop asks PP 客服 (sends the question; the answer arrives in that chat).
   */
  void OnAskSettingHelp(const std::string& label_key);
  /** Shell layout / navigation. Clear via BindShellNavigation({}). */
  void BindShellNavigation(ShellNavigationPorts ports);
  /** Toast / dialog feedback. Clear via BindShellFeedback({}). */
  void BindShellFeedback(ShellFeedbackPorts ports);
  /** Developer builds only: Settings -> About -> Engine test. Clear via BindOpenUiPerfTest({}). */
  void BindOpenUiPerfTest(std::function<void()> open);
  /** Phones: Me -> Appearance -> Home widgets opens the Edit Home sheet (ChatController). */
  void BindOpenHomeEdit(std::function<void()> open);
  void BindUnlockEnsure(UnlockEnsurePorts ports);
  SettingsCommands& Commands();
  const SettingsCommands& Commands() const;
  bool RegisterModel(ui::Context* context);
  void OnNavTabActivated();
  /** Persist nickname / dirty sections when leaving Me (tab or sheet). */
  void OnMeSurfaceClosed();
  void SyncLayoutMode();
  /** Clear detail UI without touching the shell local-back stack (used by sheet dismiss). */
  void ApplyBackToListUi();
  void OnDetailDismissed();
  /** Re-reads the "Blocked" row's count (the blocked list closed, or a person was blocked or unblocked). */
  void RefreshBlockedCount();
  /** Phones: the open section's mount was emptied by a shell remount (a page pushed over it and popped). */
  bool SheetMountEmpty() const;
  /** Phones: Account & security is the open section (change PIN, recovery phrase: the AI dot keeps off). */
  bool ShowsSecuritySection() const { return show_detail_ && selected_id_ == "security"; }
  /** Me → Account & security shows the recovery phrase now (it can be read, not only typed). */
  bool ShowsRecoveryPhrase() const { return ShowsSecuritySection() && security_actions_.RecoveryPhraseShown(); }
  void OnShellLayoutSynced();
  void Tick();
  /** Rebuild localized section titles / bindings after UI language changes. */
  void RefreshLocalizedChrome();
  /** Refresh PP Support Me-row from client-compat discovery (via SettingsCommands). */
  void SyncSupportDiscovery();
  /** Deep-link: select Me (if needed) and open the Network section. */
  void OpenNetworkSettings();
  /** Me → `section_id` from elsewhere in the app (Home's Security widget opens "security"). */
  void OpenSection(const std::string& section_id);
  /** Refresh reachability Connection card via SettingsCommands ports. */
  void SyncReachability();
  /** Persist skip/ack for the Me → Network reachability nudge (condition-keyed). */
  void AckReachabilityNudge(const std::string& status_key);

private:
  struct SettingsBindings {
    ui::String llm_preset = "brief";
    ui::String llm_base_url;
    ui::String llm_model;
    ui::String llm_api_key;
    ui::String llm_api_key_env;
    ui::String promoted_mcp_url;
    ui::String search_provider = "duckduckgo";
    std::vector<McpServerRow> mcp_servers;
    ui::String relay_base_url;
    ui::String directory_base_url;
    ui::String registration_base_url;
    ui::String node_enabled = "on";
    bool show_node_toggle = true;
    ui::String amp_listen_multiaddr;
    ui::String mesh_status_message;
    ui::String reachability_status_label;
    ui::String reachability_summary;
    ui::String reachability_help_kind;
    bool show_connection_card = false;
    bool show_reachability_help = false;
    ui::String circuit_relay_enabled = "off";
    bool show_circuit_relay_toggle = false;
    ui::String media_relay_enabled = "on";
    bool show_media_relay_toggle = false;
    ui::String dht_enabled = "off";
    bool show_dht_toggle = false;
    ui::String prefer_contacts_for_routing = "on";
    ui::String backend_env = "prod";
    ui::String backend_label;
    bool backend_restart_pending = false;
    bool backend_is_custom = false;
    /** Hidden in the UI demo app (BackendSwitchVisible). */
    bool show_backend_switch = true;
    ui::String direct_connections = "contacts";
    ui::String direct_connections_label;
    ui::String trusted_relays_only = "off";
    ui::String reachable_without_relay = "off";
    bool show_prefer_contacts_toggle = false;
    ui::String profile_nickname;
    ui::String profile_peer_id;
    ui::String profile_relay_id;
    ui::String profile_registered = "no";
    ui::String profile_registration_status = "not registered";
    ui::String profile_registration_expires;
    ui::String profile_register_label = "Register on network";
    bool profile_show_register = true;
    bool profile_show_rotate = false;
    ui::String profile_icon_src;
    bool profile_has_icon = false;
    bool profile_icon_uploading = false;
    bool profile_show_clear_icon = false;
    ui::String profile_avatar_letter = "?";
    int profile_avatar_tone = 0;
    ui::String auto_renew_registration = "auto";
    ui::String show_notifications = "on";
    ui::String show_notification_preview = "on";
    ui::String stay_connected = "off";
    /** The "Stay connected" row shows (Android: phones without a push service). */
    bool stay_connected_available = false;
    /** kDevToolsBuild: the About page shows the developer-only rows. */
    bool dev_tools_available = false;
    ui::String brief_llm_key_masked;
    ui::String appearance = "system";
    ui::String appearance_label = "System";
    ui::String language = "system";
    ui::String language_label = "System";
    ui::String reduce_transparency = "off";
    ui::String call_diagnostics = "off";
    ui::String home_show_dynamic = "off";
    ui::String crash_reports_enabled = "off";
    ui::String show_advanced_settings = "off";
    /** show_advanced_settings == "on"; templates gate technical rows on it. */
    bool advanced = false;
    ui::String profile_label;
    ui::String config_dir;
    ui::String data_dir;
    ui::String profile_dir;
    ui::String attachment_download_policy = "smart";
    ui::String attachment_download_policy_label;
    ui::String attachment_auto_on_wifi = "on";
    ui::String attachment_auto_on_cellular = "off";
    /** Phone and "everything" chosen: the Wi-Fi / mobile-network ticks show. */
    bool attachment_auto_toggles_visible = false;
    ui::String pin_protection_status;
    bool pin_protection_is_default = false;
    bool pin_protection_not_setup = false;
    bool security_can_change_pin = false;
    bool security_can_export_link = false;
    bool security_has_recovery_phrase = false;
    /** PIN grace window (#21): the minutes (not bound) and the row's label; the row shows when grace_shown. */
    int unlock_grace_minutes = 15;
    ui::String unlock_grace_label;
    bool security_grace_shown = false;
    ui::String group_invite_policy = "contacts_only";
    ui::String group_invite_policy_label = "Contacts only";
    ui::String call_invite_policy = "everyone";
    ui::String call_invite_policy_label = "Everyone";
    ui::String tool_permissions_summary = "None saved";
    bool tool_permissions_has_saved = false;
    ui::String app_name;
    ui::String app_version;
    bool support_visible = false;
    ui::String support_display_name;
    ui::String support_subtitle;
  };

  // Data-model events with more to them than one call.
  void OnLlmPresetChanged(ui::Event& ev);
  void ToggleNodeEnabled();
  void RetestReachability();
  void TryUpnpPort();
  void ShowReachabilityHelp();
  void DismissReachabilityHelp();
  void ToggleCircuitRelay();
  void ToggleMediaRelay();
  void ToggleDht();
  void TogglePreferContacts();
  void ToggleReachableWithoutRelay();
  void ToggleTrustedRelays();
  void ToggleShowNotifications();
  void ToggleShowNotificationPreview();
  void ToggleStayConnected();
  /** Me → Appearance → Transparency: flips the reduce_transparency pref (the switch shows it inverted). */
  void ToggleTransparency();
  void ToggleCallDiagnostics();
  void ToggleHomeShowDynamic();
  void ToggleShowAdvancedSettings();
  /** Me → AI assistant: the AI dot's switch, look (`a`–`d`) and size (`s` / `m` / `l`), saved to the profile prefs at once. */
  void ToggleAiDot();
  void SetAiDotLook(const std::string& look);
  /** "Assistant size": the pushed choice page (phones) with small / medium / large. */
  void OnChooseAiDotSize(ui::Event& ev);
  void SaveAiDot(bool shown, const std::string& look, const std::string& size);
  void ToggleCrashReports();
  void ToggleAutoRenewRegistration();


  void InitSections();
  SettingsSectionHandler* FindHandler(const std::string& section_id);
  const SettingsSectionHandler* FindHandler(const std::string& section_id) const;
  void PullBindingsToUiState();
  void PushUiStateToBindings();
  void ReloadFromDisk();
  void SyncBindingsFromSession();
  void ApplyReachability();
  void ApplySupportDiscovery();
  void ApplySectionAttention();
  bool ComputeNetworkAttention() const;
  // SettingsHost
  SessionStore& Store() override;
  void ShowStatus(const std::string& status) override;
  void Resync(bool include_profile_nickname) override;
  SettingsUiState& UiState() override;
  void PullFromView() override;
  void PushToView(bool include_profile_nickname) override;
  void SaveProfileNickname() override;
  void RequestSyncLayout(const char* reason) override;
  Roe<void> ForgetToolPermissions() override;
  void OpenChangePin() override;
  void FinishPaneResync();
  /** Mount selected section RML into #settings-section-mount (one section at a time). */
  void MountSelectedSettingsSection();
  void OnSelectSection(const std::string& section_id);
  void OpenSettingsDetailPane();
  bool CloseSettingsDetailPane();
  void OnBackToList();
  /** Phones: Security's own pages ("change_pin", "transfer"), pushed over the section with the local-back stack. */
  void OpenSecurityPage(const std::string& page);
  void OnCloseSecurityPage();
  /** Drops the open security page (state only; the local-back entry is the caller's). */
  void ApplyCloseSecurityPage();
  /** Closes the security page with no animation and forgets its local-back entry (the detail is going away). */
  void ResetSecurityPage();
  void OnResetSection(const std::string& section_id);
  void PerformResetSection(const std::string& section_id);
  /** `show_toast=false` for switches: the knob moving is the feedback (Kenneth 2026-10-14). */
  void MarkSectionDirty(const std::string& section_id, bool show_toast = true);
  void FlushPending();
  void FlushAllDirty();
  bool FlushSection(const std::string& section_id, bool show_toast = true);
  /** Blur / leave-Me: flush nickname only when it differs from last loaded/saved. */
  void CommitProfileNickname(bool show_toast = false);
  void MaybeShowSaveToast(const std::string& section_id);
  void ReportFailure(const Error& err) override;
  void ReportFailure(const std::string& technical_message);
  void DirtyAll(bool include_profile_nickname = true);
  void OnOpenSupportChat();
  void OnAddMcpServer();
  void OnRemoveMcpServer(int index);
  /**
   * A row that picks one value: on a phone the choice page (SettingsChoicePage, pushed like Security's pages), on the
   * desktop the float menu under the row as before. `apply` is the same path for both.
   */
  void ShowChoice(ui::Event& ev, const std::string& kind, const std::string& title, const std::string& foot,
                  const std::string& help_topic, std::vector<SettingsChoicePage::Option> options,
                  std::function<void(const std::string&)> apply);
  void OnPickChoice(const std::string& id);
  void OnChooseTheme(ui::Event& ev);
  void ApplyThemeChoice(const std::string& appearance_pref);
  void OnChooseLanguage(ui::Event& ev);
  void ApplyLanguageChoice(const std::string& language_pref);
  void OnChooseGroupInvitePolicy(ui::Event& ev);
  void OnChooseBackend(ui::Event& ev);
  void OnChooseDirectConnections(ui::Event& ev);
  void OnChooseCallInvitePolicy(ui::Event& ev);
  void ApplyCallInvitePolicyChoice(const std::string& policy);
  /** Me → Security → "Ask for PIN again after": every time / 5 / 15 / 60 minutes (#21). */
  void OnChooseUnlockGrace(ui::Event& ev);
  void ApplyUnlockGraceChoice(const std::string& minutes_id);
  void ApplyBackendChoice(const std::string& env);
  void ApplyDirectConnectionsChoice(const std::string& audience);
  void ApplyGroupInvitePolicyChoice(const std::string& policy);
  void ApplyAttachmentDownloadPolicyChoice(const std::string& policy);
  void ToggleAttachmentAutoNetwork(bool cellular);

  ShellChromeSnapshot ChromeSnapshot() const;

  std::vector<std::unique_ptr<SettingsSectionHandler>> section_handlers_;
  std::unordered_map<std::string, SettingsSectionHandler*> section_handlers_by_id_;
  std::vector<SectionListRow> sections_;
  ui::String selected_id_;
  ui::String selected_title_;
  bool show_detail_ = false;
  /**
   * Phones: the page pushed over the section ("" none; Security's "change_pin", "transfer"; "choice", a pick-one-value
   * page of any section) and its title.
   */
  ui::String security_page_;
  ui::String security_page_title_;
  SettingsChoicePage choice_page_;
  ui::String choice_foot_;
  ui::String choice_help_topic_;
  /** Each pick schedules the page's way back; an older schedule finding a newer pick (or another page) does nothing. */
  uint64_t choice_pop_generation_ = 0;
  /** Security's "Blocked" row: how many people are blocked (0: no number shown). */
  int blocked_count_ = 0;
  /** Me → AI assistant (phones): the AI dot's switch, its look and size (with the size's name for its row). */
  bool ai_dot_shown_ = true;
  ui::String ai_dot_look_ = "b";
  ui::String ai_dot_size_ = "m";
  ui::String ai_dot_size_label_;
  bool compact_layout_ = false;
  SettingsUiState ui_state_;
  SettingsBindings bindings_;
  ui::String status_;
  ui::Context* context_ = nullptr;
  /** Pane hydrate gate; also mirrored into UiEditSession remount depth. */
  bool suppress_auto_save_ = false;
  std::unordered_set<std::string> dirty_sections_;
  /** Dirty sections whose next flush must not toast (switch toggles). */
  std::unordered_set<std::string> quiet_sections_;
  uint64_t debounce_deadline_ms_ = 0;
  std::optional<std::string> last_toast_section_;
  uint64_t last_toast_at_ms_ = 0;
  SettingsCommands commands_;
  ShellNavigationPorts shell_navigation_;
  ShellFeedbackPorts shell_feedback_;
  std::function<void()> open_ui_perf_test_;
  std::function<void()> open_home_edit_;
  UnlockEnsurePorts unlock_ensure_;
  SettingsCasLibrary cas_library_{commands_, shell_feedback_, unlock_ensure_,
                                  [this](const Error& err) { ReportFailure(err); }};
  SettingsSecurityActions security_actions_{*this, commands_, shell_feedback_, unlock_ensure_};
  SettingsOwnDevices own_devices_{commands_, shell_feedback_, [this](const Error& err) { ReportFailure(err); }};
  SettingsProfileActions profile_actions_{*this, commands_, unlock_ensure_};
  StorageManagerController storage_manager_{commands_, shell_navigation_, shell_feedback_, [this]() {
    const ShellChromeSnapshot chrome = ChromeSnapshot();
    return selected_id_ == "storage" && chrome.nav_tab == NavTab::Me;
  }};

};

/**
 * What a "?" beside a setting asks PP 客服: "What does “<label>” do?" in the UI language. `label_key` is the
 * setting's own label key (a "settings." key); anything else asks nothing ("").
 */
std::string SettingHelpQuestion(const std::string& label_key);

} // namespace pbr
