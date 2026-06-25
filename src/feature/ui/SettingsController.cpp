#include "feature/ui/SettingsController.h"

#include "base/data/AppPaths.h"
#include "base/data/LlmPreset.h"
#include "base/data/SessionStore.h"
#include "feature/ui/DataModelHost.h"
#include "feature/ui/ShellFeedback.h"
#include "feature/ui/ShellHost.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>

namespace pbr {

namespace {

Rml::String EventValue(Rml::Event& ev) {
  return ev.GetParameter<Rml::String>("value", Rml::String());
}

} // namespace

SettingsController::SettingsController() {
  redirectLogger("SettingsController");
}

SettingsController& SettingsController::Instance() {
  static SettingsController controller;
  return controller;
}

void SettingsController::LoadDraftFromSession() {
  const BootstrapResult& bootstrap = SessionStore::Instance().Snapshot();
  const AppConfig& config = bootstrap.config;

  draft_.llm_preset = ResolvePreset(config);
  draft_.llm_base_url = config.llm.base_url;
  draft_.llm_model = config.llm.model;
  draft_.llm_api_key = config.llm.api_key;
  draft_.llm_api_key_env = config.llm_api_key_env;
  draft_.appearance = bootstrap.profile_prefs.appearance;

  display_.profile_label = bootstrap.profile_registry.ActiveProfileId();
  display_.config_dir = AppPaths::ConfigDir();
  display_.data_dir = bootstrap.data_dir;
  display_.profile_dir = bootstrap.profile_data_dir;
  status_ = "";
}

bool SettingsController::RegisterModel(Rml::Context* context) {
  if (!context) {
    return false;
  }
  context_ = context;

  return DataModelHost::Instance().Register(context, "settings", [](Rml::DataModelConstructor& ctor) {
    auto& controller = SettingsController::Instance();
    ctor.Bind("llm_preset", &controller.draft_.llm_preset);
    ctor.Bind("llm_base_url", &controller.draft_.llm_base_url);
    ctor.Bind("llm_model", &controller.draft_.llm_model);
    ctor.Bind("llm_api_key", &controller.draft_.llm_api_key);
    ctor.Bind("llm_api_key_env", &controller.draft_.llm_api_key_env);
    ctor.Bind("appearance", &controller.draft_.appearance);
    ctor.Bind("profile_label", &controller.display_.profile_label);
    ctor.Bind("config_dir", &controller.display_.config_dir);
    ctor.Bind("data_dir", &controller.display_.data_dir);
    ctor.Bind("profile_dir", &controller.display_.profile_dir);
    ctor.Bind("status", &controller.status_);
    ctor.BindEventCallback("save_settings", &SettingsController::SaveSettingsCallback);
    ctor.BindEventCallback("reset_to_defaults", &SettingsController::ResetDefaultsCallback);
    ctor.BindEventCallback("draft_llm_model_changed", &SettingsController::DraftLlmModelChangedCallback);
    ctor.BindEventCallback("draft_llm_base_url_changed", &SettingsController::DraftLlmBaseUrlChangedCallback);
    ctor.BindEventCallback("draft_appearance_changed", &SettingsController::DraftAppearanceChangedCallback);
    ctor.BindEventCallback("draft_llm_api_key_env_changed", &SettingsController::DraftLlmApiKeyEnvChangedCallback);
    ctor.BindEventCallback("draft_llm_preset_changed", &SettingsController::DraftLlmPresetChangedCallback);
    ctor.BindEventCallback("open_settings", &SettingsController::OpenSettingsCallback);
  });
}

void SettingsController::DirtyAll() {
  auto& host = DataModelHost::Instance();
  host.Dirty("settings", "llm_preset");
  host.Dirty("settings", "llm_base_url");
  host.Dirty("settings", "llm_model");
  host.Dirty("settings", "llm_api_key");
  host.Dirty("settings", "llm_api_key_env");
  host.Dirty("settings", "appearance");
  host.Dirty("settings", "profile_label");
  host.Dirty("settings", "config_dir");
  host.Dirty("settings", "data_dir");
  host.Dirty("settings", "profile_dir");
  host.Dirty("settings", "status");
}

void SettingsController::OnSettingsMounted() {
  DirtyAll();
  if (context_) {
    context_->Update();
  }
  suppress_preset_change_ = false;
}

void SettingsController::OpenSettings() {
  LoadDraftFromSession();
  ShellHost::Instance().SetOnBeforeTransientMount([](const std::string& key) {
    if (key == "settings") {
      SettingsController::Instance().suppress_preset_change_ = true;
    }
  });
  ShellHost::Instance().SetOnTransientMounted([](const std::string& key) {
    if (key == "settings") {
      SettingsController::Instance().OnSettingsMounted();
    }
  });
  ShellHost::Instance().PushTransient({.key = "settings", .rml_path = "views/settings.rml", .toolbar_label = "Settings"});
  ShellHost::Instance().DirtyWindow();
}

SettingsDraft SettingsController::ToLogicDraft() const {
  SettingsDraft draft;
  draft.llm_preset = draft_.llm_preset.c_str();
  draft.llm_base_url = draft_.llm_base_url.c_str();
  draft.llm_model = draft_.llm_model.c_str();
  draft.llm_api_key = draft_.llm_api_key.c_str();
  draft.llm_api_key_env = draft_.llm_api_key_env.c_str();
  return draft;
}

void SettingsController::OnSaveSettings() {
  if (context_) {
    context_->Update();
  }

  const AppConfig config =
      ApplySettingsDraft(SessionStore::Instance().Snapshot().config, ToLogicDraft());

  if (auto saved = SessionStore::Instance().SaveConfig(config); !saved) {
    status_ = saved.error().message;
    DataModelHost::Instance().Dirty("settings", "status");
    return;
  }

  ProfilePreferences profile_prefs = SessionStore::Instance().Snapshot().profile_prefs;
  profile_prefs.appearance = draft_.appearance.c_str();
  if (auto saved = SessionStore::Instance().SaveProfilePrefs(profile_prefs); !saved) {
    status_ = saved.error().message;
    DataModelHost::Instance().Dirty("settings", "status");
    return;
  }

  LoadDraftFromSession();
  status_ = "Settings saved";
  DirtyAll();
  ShellFeedback::ShowToast(ShellHost::Instance().State(), "Settings saved");
  ShellHost::Instance().DirtyWindow();
}

void SettingsController::OnResetDefaults() {
  const AppConfig defaults = SessionStore::Instance().DefaultConfig();
  const ProfilePreferences default_prefs = SessionStore::Instance().DefaultProfilePrefs();

  draft_.llm_preset = ResolvePreset(defaults);
  draft_.llm_base_url = defaults.llm.base_url;
  draft_.llm_model = defaults.llm.model;
  draft_.llm_api_key = defaults.llm.api_key;
  draft_.llm_api_key_env = defaults.llm_api_key_env;
  draft_.appearance = default_prefs.appearance;

  DirtyAll();
  status_ = "Defaults loaded (save to persist)";
  DataModelHost::Instance().Dirty("settings", "status");
}

void SettingsController::SaveSettingsCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                               const Rml::VariantList& /*args*/) {
  Instance().OnSaveSettings();
}

void SettingsController::ResetDefaultsCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                const Rml::VariantList& /*args*/) {
  Instance().OnResetDefaults();
}

void SettingsController::DraftLlmModelChangedCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev,
                                                      const Rml::VariantList& /*args*/) {
  Instance().draft_.llm_model = EventValue(ev);
}

void SettingsController::DraftLlmBaseUrlChangedCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev,
                                                        const Rml::VariantList& /*args*/) {
  Instance().draft_.llm_base_url = EventValue(ev);
}

void SettingsController::DraftAppearanceChangedCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev,
                                                        const Rml::VariantList& /*args*/) {
  Instance().draft_.appearance = EventValue(ev);
}

void SettingsController::DraftLlmApiKeyEnvChangedCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev,
                                                          const Rml::VariantList& /*args*/) {
  Instance().draft_.llm_api_key_env = EventValue(ev);
}

void SettingsController::DraftLlmPresetChangedCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev,
                                                       const Rml::VariantList& /*args*/) {
  auto& controller = Instance();
  if (controller.suppress_preset_change_) {
    return;
  }
  controller.draft_.llm_preset = EventValue(ev);
}

void SettingsController::OpenSettingsCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                              const Rml::VariantList& /*args*/) {
  Instance().OpenSettings();
}

} // namespace pbr
