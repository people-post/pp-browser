#pragma once

#include "foundation/data/SessionStore.h"
#include "feature/settings/SettingsCommands.h"
#include "feature/settings/SettingsSectionHandler.h"
#include "feature/settings/SettingsSections.h"
#include "feature/settings/SettingsUiState.h"
#include "gui/SettingsCasLibrary.h"
#include "gui/SettingsHost.h"
#include "gui/SettingsProfileActions.h"
#include "gui/SettingsSecurityActions.h"
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
  /** Shell layout / navigation without ShellHost::Instance(). Clear via BindShellNavigation({}). */
  void BindShellNavigation(ShellNavigationPorts ports);
  /** Toast / dialog feedback without ShellHost::Instance(). Clear via BindShellFeedback({}). */
  void BindShellFeedback(ShellFeedbackPorts ports);
  void BindUnlockEnsure(UnlockEnsurePorts ports);
  SettingsCommands& Commands();
  const SettingsCommands& Commands() const;
  bool RegisterModel(ui::Context* context);
  void OnNavTabActivated();
  /** Persist nickname / dirty sections when leaving Me (tab or sheet). */
  void OnMeSurfaceClosed();
  void SyncLayoutMode();
  void OnAccountSheetOpened();
  void OnAccountSheetClosed();
  /** Clear detail UI without touching the shell local-back stack (used by sheet dismiss). */
  void ApplyBackToListUi();
  void OnDetailDismissed();
  void OnShellLayoutSynced();
  void Tick();
  /** Rebuild localized section titles / bindings after UI language changes. */
  void RefreshLocalizedChrome();
  /** Refresh PP Support Me-row from client-compat discovery (via SettingsCommands). */
  void SyncSupportDiscovery();
  /** Deep-link: select Me (if needed) and open the Network section. */
  void OpenNetworkSettings();
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
    ui::String direct_connections = "contacts";
    ui::String direct_connections_label;
    ui::String trusted_relays_only = "off";
    ui::String reachable_without_relay = "off";
    bool show_prefer_contacts_toggle = false;
    ui::String profile_nickname;
    ui::String profile_peer_id;
    ui::String profile_relay_id;
    ui::String profile_public_key;
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
    ui::String stay_connected = "off";
    /** The "Stay connected" row shows (Android: phones without a push service). */
    bool stay_connected_available = false;
    ui::String brief_llm_key_masked;
    ui::String appearance = "system";
    ui::String appearance_label = "System";
    ui::String language = "system";
    ui::String language_label = "System";
    ui::String reduce_transparency = "off";
    ui::String call_diagnostics = "off";
    ui::String crash_reports_enabled = "off";
    ui::String show_advanced_settings = "off";
    /** show_advanced_settings == "on"; templates gate technical rows on it. */
    bool advanced = false;
    ui::String profile_label;
    ui::String config_dir;
    ui::String data_dir;
    ui::String profile_dir;
    ui::String profile_size_label;
    ui::String attachment_cache_size_label;
    ui::String attachment_download_policy = "smart";
    ui::String attachment_download_policy_label;
    ui::String pin_protection_status;
    bool pin_protection_is_default = false;
    bool pin_protection_not_setup = false;
    bool security_can_change_pin = false;
    bool security_can_export_link = false;
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
  void ToggleStayConnected();
  void ToggleReduceTransparency();
  void ToggleCallDiagnostics();
  void ToggleShowAdvancedSettings();
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
  void FinishPaneResync();
  /** Mount selected section RML into #settings-section-mount (one section at a time). */
  void MountSelectedSettingsSection();
  void OnSelectSection(const std::string& section_id);
  void OpenSettingsDetailPane();
  bool CloseSettingsDetailPane();
  void OnBackToList();
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
  void OnChooseTheme(ui::Event& ev);
  void ApplyThemeChoice(const std::string& appearance_pref);
  void OnChooseLanguage(ui::Event& ev);
  void ApplyLanguageChoice(const std::string& language_pref);
  void OnChooseGroupInvitePolicy(ui::Event& ev);
  void OnChooseDirectConnections(ui::Event& ev);
  void OnChooseCallInvitePolicy(ui::Event& ev);
  void ApplyCallInvitePolicyChoice(const std::string& policy);
  void ApplyDirectConnectionsChoice(const std::string& audience);
  void ApplyGroupInvitePolicyChoice(const std::string& policy);
  void OnChooseAttachmentDownloadPolicy(ui::Event& ev);
  void ApplyAttachmentDownloadPolicyChoice(const std::string& policy);
  void OnDrainPendingAttachmentMedia();
  void OnClearDownloadedAttachments();
  void PerformClearDownloadedAttachments();

  ShellChromeSnapshot ChromeSnapshot() const;

  std::vector<std::unique_ptr<SettingsSectionHandler>> section_handlers_;
  std::unordered_map<std::string, SettingsSectionHandler*> section_handlers_by_id_;
  std::vector<SectionListRow> sections_;
  ui::String selected_id_;
  ui::String selected_title_;
  bool in_account_sheet_ = false;
  bool show_detail_ = false;
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
  UnlockEnsurePorts unlock_ensure_;
  SettingsCasLibrary cas_library_{commands_, shell_feedback_, unlock_ensure_,
                                  [this](const Error& err) { ReportFailure(err); }};
  SettingsSecurityActions security_actions_{*this, commands_, shell_feedback_, unlock_ensure_};
  SettingsProfileActions profile_actions_{*this, commands_, unlock_ensure_};

};

} // namespace pbr
