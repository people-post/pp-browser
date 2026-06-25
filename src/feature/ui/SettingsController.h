#pragma once

#include "base/data/BootstrapTypes.h"
#include "feature/settings/SettingsLogic.h"
#include "common/Module.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Types.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace Rml {
class Context;
}

namespace pbr {

class SettingsController : public Module {
public:
  enum class SettingsBlock {
    Llm,
    Appearance,
  };

  struct SectionListRow {
    Rml::String id;
    Rml::String title;
    Rml::String subtitle;
  };

  static SettingsController& Instance();

  bool RegisterModel(Rml::Context* context);
  void OpenSettings();
  void OnNavTabActivated();
  void OnNavTabDeactivated();
  void SyncLayoutMode();
  void Tick();

private:
  struct SettingsUiDraft {
    Rml::String llm_preset = "cloud";
    Rml::String llm_base_url;
    Rml::String llm_model;
    Rml::String llm_api_key;
    Rml::String llm_api_key_env;
    Rml::String appearance = "system";
  };

  struct SettingsDisplay {
    Rml::String profile_label;
    Rml::String config_dir;
    Rml::String data_dir;
    Rml::String profile_dir;
  };

  SettingsController();

  static void SelectSectionCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void BackToListCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ResetSectionCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnLlmFieldChangedCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnLlmPresetChangedCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnAppearanceChangedCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);

  void InitSections();
  void ReloadFromDisk();
  void SyncBindingsFromSession();
  void OnSettingsMounted();
  void OnSelectSection(const std::string& section_id);
  void OnBackToList();
  void OnResetSection(const std::string& section_id);
  void ScheduleBlockFlush(SettingsBlock block);
  void FlushPending();
  bool FlushBlock(SettingsBlock block);
  void MaybeShowSaveToast(SettingsBlock block);
  void DirtyAll();
  void CompleteSectionSelection(bool expanded);
  SettingsDraft ToLogicDraft() const;

  std::vector<SectionListRow> sections_;
  Rml::String selected_id_;
  Rml::String selected_title_;
  bool compact_layout_ = false;
  bool show_detail_ = false;
  SettingsUiDraft draft_;
  SettingsDisplay display_;
  Rml::String status_;
  Rml::Context* context_ = nullptr;
  bool suppress_auto_save_ = false;
  std::optional<SettingsBlock> pending_flush_block_;
  uint64_t pending_flush_at_ms_ = 0;
  std::optional<SettingsBlock> last_toast_block_;
  uint64_t last_toast_at_ms_ = 0;
};

} // namespace pbr
