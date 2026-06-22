#pragma once

#include "app/Bootstrap.h"
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
  void LoadFromSession();

  static void OpenSettingsCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);

private:
  struct SettingsState {
    Rml::String llm_preset = "cloud";
    Rml::String llm_base_url;
    Rml::String llm_model;
    Rml::String llm_api_key;
    Rml::String llm_api_key_env;
    Rml::String theme;
    Rml::String profile_label;
    Rml::String config_dir;
    Rml::String data_dir;
    Rml::String profile_dir;
    Rml::String status;
  };

  SettingsController();

  static void SaveSettingsCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ResetDefaultsCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ApplyLlmPresetCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);

  void OnSaveSettings();
  void OnResetDefaults();
  void OnApplyLlmPreset(const std::string& preset);
  void DirtyAll();
  void SchedulePostMountRefresh();

  AppConfig BuildConfigFromDraft() const;

  SettingsState state_;
  bool suppress_preset_apply_ = false;
  Rml::Context* context_ = nullptr;
};

} // namespace pbr
