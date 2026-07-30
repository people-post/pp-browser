#pragma once

#include "base/data/SessionStore.h"
#include "feature/settings/SettingsCommands.h"
#include "feature/settings/SettingsSectionHandler.h"
#include "feature/settings/SettingsSections.h"
#include "feature/settings/SettingsUiState.h"
#include "common/Error.h"
#include "common/Module.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Types.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rml {
class Context;
}

namespace pbr {

class SettingsController : public Module {
public:
  struct SectionListRow {
    Rml::String id;
    Rml::String title;
    Rml::String subtitle;
  };

  struct McpServerRow {
    Rml::String id;
    Rml::String url;
    Rml::String command;
    Rml::String args_text;
    bool enabled = true;
  };

  static SettingsController& Instance();

  /** App fills ports (session, messaging views, register, UPnP, …). Not a process singleton. */
  void BindCommands(SettingsCommands commands);
  SettingsCommands& Commands();
  const SettingsCommands& Commands() const;
  bool RegisterModel(Rml::Context* context);
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
  /** Refresh reachability Connection card via SettingsCommands ports. */
  void SyncReachability();

private:
  struct SettingsBindings {
    Rml::String llm_preset = "brief";
    Rml::String llm_base_url;
    Rml::String llm_model;
    Rml::String llm_api_key;
    Rml::String llm_api_key_env;
    Rml::String promoted_mcp_url;
    Rml::String search_provider = "duckduckgo";
    std::vector<McpServerRow> mcp_servers;
    Rml::String relay_base_url;
    Rml::String directory_base_url;
    Rml::String registration_base_url;
    Rml::String node_enabled = "on";
    bool show_node_toggle = true;
    Rml::String libp2p_listen_multiaddr;
    Rml::String libp2p_status_message;
    Rml::String reachability_status_label;
    Rml::String reachability_summary;
    Rml::String reachability_help_kind;
    bool show_connection_card = false;
    bool show_reachability_help = false;
    Rml::String circuit_relay_enabled = "off";
    bool show_circuit_relay_toggle = false;
    Rml::String media_relay_enabled = "on";
    bool show_media_relay_toggle = false;
    Rml::String prefer_contacts_for_routing = "on";
    bool show_prefer_contacts_toggle = false;
    Rml::String profile_nickname;
    Rml::String profile_peer_id;
    Rml::String profile_relay_id;
    Rml::String profile_public_key;
    Rml::String profile_registered = "no";
    Rml::String profile_registration_status = "not registered";
    Rml::String profile_registration_expires;
    Rml::String profile_register_label = "Register on network";
    bool profile_show_register = true;
    bool profile_show_rotate = false;
    Rml::String auto_renew_registration = "auto";
    Rml::String show_notifications = "on";
    Rml::String brief_llm_key_masked;
    Rml::String appearance = "system";
    Rml::String appearance_label = "System";
    Rml::String language = "system";
    Rml::String language_label = "System";
    Rml::String reduce_transparency = "off";
    Rml::String profile_label;
    Rml::String config_dir;
    Rml::String data_dir;
    Rml::String profile_dir;
    Rml::String profile_size_label;
    Rml::String pin_protection_status;
    bool security_can_change_pin = false;
    Rml::String pin_change_old;
    Rml::String pin_change_new;
    Rml::String pin_change_confirm;
    Rml::String group_invite_policy = "contacts_only";
    Rml::String group_invite_policy_label = "Contacts only";
    Rml::String app_name;
    Rml::String app_version;
  };

  SettingsController();

  static void SelectSectionCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void BackToListCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ResetSectionCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnLlmFieldChangedCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnLlmPresetChangedCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnChooseThemeCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnChooseLanguageCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnChooseGroupInvitePolicyCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ToggleShowNotificationsCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ToggleReduceTransparencyCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ToggleAutoRenewRegistrationCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnIntegrationsFieldChangedCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnNetworkFieldChangedCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ToggleNodeEnabledCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void RetestReachabilityCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void TryUpnpPortCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ShowReachabilityHelpCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void DismissReachabilityHelpCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ToggleCircuitRelayCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ToggleMediaRelayCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void TogglePreferContactsCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnProfileNicknameCommitCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnRegisterProfileCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnRotateBriefLlmKeyCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnCopyProfileIdCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnShareProfileCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnAddMcpServerCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnRemoveMcpServerCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnChangePinCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnClearUndeliveredCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnResetProfileCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);

  void InitSections();
  SettingsSectionHandler* FindHandler(const std::string& section_id);
  const SettingsSectionHandler* FindHandler(const std::string& section_id) const;
  void PullBindingsToUiState();
  void PushUiStateToBindings();
  void ReloadFromDisk();
  void SyncBindingsFromSession();
  void ApplyReachability();
  SessionStore& Store();
  void FinishPaneResync();
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
  void OnShareProfile();
  void OnAddMcpServer();
  void OnRemoveMcpServer(int index);
  void OnChangePin();
  void OnClearUndeliveredOlderThan();
  void OnResetProfile();
  void PerformResetProfile();
  void OnChooseTheme(Rml::Event& ev);
  void ApplyThemeChoice(const std::string& appearance_pref);
  void OnChooseLanguage(Rml::Event& ev);
  void ApplyLanguageChoice(const std::string& language_pref);
  void OnChooseGroupInvitePolicy(Rml::Event& ev);
  void ApplyGroupInvitePolicyChoice(const std::string& policy);

  std::vector<std::unique_ptr<SettingsSectionHandler>> section_handlers_;
  std::unordered_map<std::string, SettingsSectionHandler*> section_handlers_by_id_;
  std::vector<SectionListRow> sections_;
  Rml::String selected_id_;
  Rml::String selected_title_;
  bool in_account_sheet_ = false;
  bool show_detail_ = false;
  bool compact_layout_ = false;
  SettingsUiState ui_state_;
  SettingsBindings bindings_;
  Rml::String status_;
  Rml::Context* context_ = nullptr;
  /** Pane hydrate gate; also mirrored into UiEditSession remount depth. */
  bool suppress_auto_save_ = false;
  std::unordered_set<std::string> dirty_sections_;
  uint64_t debounce_deadline_ms_ = 0;
  std::optional<std::string> last_toast_section_;
  uint64_t last_toast_at_ms_ = 0;
  SettingsCommands commands_;

};

} // namespace pbr
