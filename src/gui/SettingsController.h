#pragma once

#include "foundation/data/SessionStore.h"
#include "feature/settings/SettingsCommands.h"
#include "feature/settings/SettingsSectionHandler.h"
#include "feature/settings/SettingsSections.h"
#include "feature/settings/SettingsUiState.h"
#include "gui/shell/ShellFeedbackPorts.h"
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

class SettingsController : public Module {
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

  struct CasLibraryRow {
    ui::String content_id_hex;
    ui::String title;
    ui::String detail;
    ui::String realm_label;
    ui::String pin_label;
    bool can_share_publicly = false;
    bool can_unpublish = false;
    bool can_copy_tip = false;
  };

  SettingsController();
  ~SettingsController() override = default;

  /** App-owned instance; set via InstallInstance from Application. Static callbacks use Instance(). */
  static void InstallInstance(SettingsController& controller);
  static void ClearInstance();
  static SettingsController& Instance();

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
    std::vector<CasLibraryRow> cas_library_rows;
    ui::String cas_library_empty_label;
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
    ui::String brief_llm_key_masked;
    ui::String appearance = "system";
    ui::String appearance_label = "System";
    ui::String language = "system";
    ui::String language_label = "System";
    ui::String reduce_transparency = "off";
    ui::String call_diagnostics = "off";
    ui::String crash_reports_enabled = "off";
    ui::String profile_label;
    ui::String config_dir;
    ui::String data_dir;
    ui::String profile_dir;
    ui::String profile_size_label;
    ui::String attachment_cache_size_label;
    ui::String attachment_download_policy = "smart";
    ui::String attachment_download_policy_label;
    ui::String pin_protection_status;
    bool security_can_change_pin = false;
    bool security_can_export_link = false;
    ui::String pin_change_old;
    ui::String pin_change_new;
    ui::String pin_change_confirm;
    ui::String group_invite_policy = "contacts_only";
    ui::String group_invite_policy_label = "Contacts only";
    ui::String tool_permissions_summary = "None saved";
    bool tool_permissions_has_saved = false;
    ui::String app_name;
    ui::String app_version;
    bool support_visible = false;
    ui::String support_display_name;
    ui::String support_subtitle;
  };

  static void SelectSectionCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void BackToListCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void ResetSectionCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnLlmFieldChangedCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnLlmPresetChangedCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnChooseThemeCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnChooseLanguageCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnChooseGroupInvitePolicyCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnChooseAttachmentDownloadPolicyCallback(ui::DataModelHandle model, ui::Event& ev,
                                                       const ui::VariantList& args);
  static void DrainPendingAttachmentMediaCallback(ui::DataModelHandle model, ui::Event& ev,
                                                  const ui::VariantList& args);
  static void ClearDownloadedAttachmentsCallback(ui::DataModelHandle model, ui::Event& ev,
                                                 const ui::VariantList& args);
  static void SetCasLibraryFilterCallback(ui::DataModelHandle model, ui::Event& ev,
                                         const ui::VariantList& args);
  static void ShareCasPubliclyCallback(ui::DataModelHandle model, ui::Event& ev,
                                      const ui::VariantList& args);
  static void UnpublishCasCallback(ui::DataModelHandle model, ui::Event& ev,
                                  const ui::VariantList& args);
  static void CopyCasTipCallback(ui::DataModelHandle model, ui::Event& ev,
                               const ui::VariantList& args);
  static void FetchCasTipCallback(ui::DataModelHandle model, ui::Event& ev,
                                const ui::VariantList& args);
  void RefreshCasLibrary();
  void PushCasLibraryBindings();
  void OnSetCasLibraryFilter(const std::string& filter);
  void OnShareCasPublicly(int index);
  void OnUnpublishCas(int index);
  void OnCopyCasTip(int index);
  void OnFetchCasTip();
  void PerformShareCasPublicly(const std::string& content_id_hex);
  void PerformUnpublishCas(const std::string& content_id_hex);
  void PerformFetchCasTip(const std::string& tip, const std::string& peer_relay_user_id);
  static void ToggleShowNotificationsCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void ToggleReduceTransparencyCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void ToggleCallDiagnosticsCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void ToggleCrashReportsCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void ToggleAutoRenewRegistrationCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnIntegrationsFieldChangedCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnNetworkFieldChangedCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void ToggleNodeEnabledCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void RetestReachabilityCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void TryUpnpPortCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void ShowReachabilityHelpCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void DismissReachabilityHelpCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void ToggleCircuitRelayCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void ToggleMediaRelayCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void ToggleDhtCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void TogglePreferContactsCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnProfileNicknameCommitCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnRegisterProfileCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnRotateBriefLlmKeyCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnCopyProfileIdCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnPickProfileIconCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnClearProfileIconCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnShareProfileCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnAddMcpServerCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnRemoveMcpServerCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnChangePinCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnExportLinkDeviceCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnClearUndeliveredCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnResetToolPermissionsCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OnResetProfileCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);
  static void OpenSupportChatCallback(ui::DataModelHandle model, ui::Event& ev, const ui::VariantList& args);

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
  SessionStore& Store();
  void FinishPaneResync();
  /** Mount selected section RML into #settings-section-mount (one section at a time). */
  void MountSelectedSettingsSection();
  void OnSelectSection(const std::string& section_id);
  void OpenSettingsDetailPane();
  bool CloseSettingsDetailPane();
  void OnBackToList();
  void OnResetSection(const std::string& section_id);
  void PerformResetSection(const std::string& section_id);
  void MarkSectionDirty(const std::string& section_id);
  void FlushPending();
  void FlushAllDirty();
  bool FlushSection(const std::string& section_id, bool show_toast = true);
  /** Blur / leave-Me: flush nickname only when it differs from last loaded/saved. */
  void CommitProfileNickname(bool show_toast = false);
  void MaybeShowSaveToast(const std::string& section_id);
  void ReportFailure(const Error& err);
  void ReportFailure(const std::string& technical_message);
  void DirtyAll(bool include_profile_nickname = true);
  void OnRegisterProfile();
  void OnRotateBriefLlmKey();
  void OnCopyProfileId();
  void OnPickProfileIcon();
  void OnClearProfileIcon();
  void OnShareProfile();
  void OnOpenSupportChat();
  void OnAddMcpServer();
  void OnRemoveMcpServer(int index);
  void OnChangePin();
  void OnExportLinkDevice();
  void EnsureSecurityUnlocked(std::function<void()> then);
  void OnClearUndeliveredOlderThan();
  void OnResetToolPermissions();
  void OnResetProfile();
  void PerformResetProfile();
  void OnChooseTheme(ui::Event& ev);
  void ApplyThemeChoice(const std::string& appearance_pref);
  void OnChooseLanguage(ui::Event& ev);
  void ApplyLanguageChoice(const std::string& language_pref);
  void OnChooseGroupInvitePolicy(ui::Event& ev);
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
  uint64_t debounce_deadline_ms_ = 0;
  std::optional<std::string> last_toast_section_;
  uint64_t last_toast_at_ms_ = 0;
  SettingsCommands commands_;
  ShellNavigationPorts shell_navigation_;
  ShellFeedbackPorts shell_feedback_;
  UnlockEnsurePorts unlock_ensure_;

  static SettingsController* installed_instance_;
};

} // namespace pbr
