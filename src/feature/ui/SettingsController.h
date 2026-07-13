#pragma once

#include "feature/settings/SettingsSectionHandler.h"
#include "feature/settings/SettingsSections.h"
#include "feature/settings/SettingsUiState.h"
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

  bool RegisterModel(Rml::Context* context);
  void OpenSettings();
  void OnNavTabActivated();
  void OnNavTabDeactivated();
  void OnShellLayoutSynced();
  void SyncLayoutMode();
  void Tick();

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
    Rml::String profile_nickname;
    Rml::String profile_peer_id;
    Rml::String profile_relay_id;
    Rml::String profile_public_key;
    Rml::String profile_registered = "no";
    Rml::String brief_llm_key_masked;
    Rml::String appearance = "system";
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
  };

  SettingsController();

  static void SelectSectionCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void BackToListCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ResetSectionCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnLlmFieldChangedCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnLlmPresetChangedCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnAppearanceChangedCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnIntegrationsFieldChangedCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnNetworkFieldChangedCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnProfileFieldChangedCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnRegisterProfileCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnRotateBriefLlmKeyCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnCopyProfileIdCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnShareProfileCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnAddMcpServerCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnRemoveMcpServerCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnChangePinCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnResetProfileCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);

  void InitSections();
  SettingsSectionHandler* FindHandler(const std::string& section_id);
  const SettingsSectionHandler* FindHandler(const std::string& section_id) const;
  void PullBindingsToUiState();
  void PushUiStateToBindings();
  void ReloadFromDisk();
  void SyncBindingsFromSession();
  void FinishPaneResync();
  void OnSelectSection(const std::string& section_id);
  void OnBackToList();
  void OnResetSection(const std::string& section_id);
  void MarkSectionDirty(const std::string& section_id);
  void FlushPending();
  void FlushAllDirty();
  bool FlushSection(const std::string& section_id);
  void MaybeShowSaveToast(const std::string& section_id);
  void DirtyAll();
  void CompleteSectionSelection(bool expanded);
  void OnRegisterProfile();
  void OnRotateBriefLlmKey();
  void OnCopyProfileId();
  void OnShareProfile();
  void OnAddMcpServer();
  void OnRemoveMcpServer(int index);
  void OnChangePin();
  void OnResetProfile();
  void PerformResetProfile();

  std::vector<std::unique_ptr<SettingsSectionHandler>> section_handlers_;
  std::unordered_map<std::string, SettingsSectionHandler*> section_handlers_by_id_;
  std::vector<SectionListRow> sections_;
  Rml::String selected_id_;
  Rml::String selected_title_;
  bool compact_layout_ = false;
  bool show_detail_ = false;
  SettingsUiState ui_state_;
  SettingsBindings bindings_;
  Rml::String status_;
  Rml::Context* context_ = nullptr;
  bool suppress_auto_save_ = false;
  std::unordered_set<std::string> dirty_sections_;
  uint64_t debounce_deadline_ms_ = 0;
  std::optional<std::string> last_toast_section_;
  uint64_t last_toast_at_ms_ = 0;
};

} // namespace pbr
