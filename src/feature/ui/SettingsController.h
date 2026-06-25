#pragma once

#include "app/Bootstrap.h"
#include "app/SettingsLogic.h"
#include "common/Module.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Types.h>

namespace Rml {
class Context;
}

namespace pbr {

class SettingsController : public Module {
public:
  static SettingsController& Instance();

  bool RegisterModel(Rml::Context* context);
  void OpenSettings();

  static void OpenSettingsCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);

private:
  struct SettingsUiDraft {
    Rml::String llm_preset = "cloud";
    Rml::String llm_base_url;
    Rml::String llm_model;
    Rml::String llm_api_key;
    Rml::String llm_api_key_env;
    Rml::String theme;
  };

  struct SettingsDisplay {
    Rml::String profile_label;
    Rml::String config_dir;
    Rml::String data_dir;
    Rml::String profile_dir;
  };

  SettingsController();

  static void SaveSettingsCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ResetDefaultsCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void DraftLlmModelChangedCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void DraftLlmBaseUrlChangedCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void DraftThemeChangedCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void DraftLlmApiKeyEnvChangedCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void DraftLlmPresetChangedCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);

  void LoadDraftFromSession();
  void OnSettingsMounted();
  void OnSaveSettings();
  void OnResetDefaults();
  void DirtyAll();
  SettingsDraft ToLogicDraft() const;

  SettingsUiDraft draft_;
  SettingsDisplay display_;
  Rml::String status_;
  Rml::Context* context_ = nullptr;
  bool suppress_preset_change_ = false;
};

} // namespace pbr
