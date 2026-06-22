#include "ui/SettingsController.h"

#include "app/AppPaths.h"
#include "app/LlmPreset.h"
#include "app/SettingsLogic.h"
#include "app/SessionStore.h"
#include "platform/BrowserThread.h"
#include "ui/DataModelHost.h"
#include "ui/ShellFeedback.h"
#include "ui/ShellHost.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>

namespace pbr {

SettingsController::SettingsController() {
  redirectLogger("SettingsController");
}

SettingsController& SettingsController::Instance() {
  static SettingsController controller;
  return controller;
}

void SettingsController::LoadFromSession() {
  const BootstrapResult& bootstrap = SessionStore::Instance().Snapshot();
  const AppConfig& config = bootstrap.config;

  suppress_preset_apply_ = true;
  state_.llm_preset = ResolvePreset(config);
  state_.llm_base_url = config.llm.base_url;
  state_.llm_model = config.llm.model;
  state_.llm_api_key = config.llm.api_key;
  state_.llm_api_key_env = config.llm_api_key_env;
  state_.theme = bootstrap.profile_prefs.theme;
  state_.profile_label = bootstrap.profile_registry.ActiveProfileId();
  state_.config_dir = AppPaths::ConfigDir();
  state_.data_dir = bootstrap.data_dir;
  state_.profile_dir = bootstrap.profile_data_dir;
  state_.status = "";
}

bool SettingsController::RegisterModel(Rml::Context* context) {
  if (!context) {
    return false;
  }
  context_ = context;

  return DataModelHost::Instance().Register(context, "settings", [](Rml::DataModelConstructor& ctor) {
    auto& controller = SettingsController::Instance();
    ctor.Bind("llm_preset", &controller.state_.llm_preset);
    ctor.Bind("llm_base_url", &controller.state_.llm_base_url);
    ctor.Bind("llm_model", &controller.state_.llm_model);
    ctor.Bind("llm_api_key", &controller.state_.llm_api_key);
    ctor.Bind("llm_api_key_env", &controller.state_.llm_api_key_env);
    ctor.Bind("theme", &controller.state_.theme);
    ctor.Bind("profile_label", &controller.state_.profile_label);
    ctor.Bind("config_dir", &controller.state_.config_dir);
    ctor.Bind("data_dir", &controller.state_.data_dir);
    ctor.Bind("profile_dir", &controller.state_.profile_dir);
    ctor.Bind("status", &controller.state_.status);
    ctor.BindEventCallback("save_settings", &SettingsController::SaveSettingsCallback);
    ctor.BindEventCallback("reset_to_defaults", &SettingsController::ResetDefaultsCallback);
    ctor.BindEventCallback("apply_llm_preset", &SettingsController::ApplyLlmPresetCallback);
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
  host.Dirty("settings", "theme");
  host.Dirty("settings", "profile_label");
  host.Dirty("settings", "config_dir");
  host.Dirty("settings", "data_dir");
  host.Dirty("settings", "profile_dir");
  host.Dirty("settings", "status");
}

void SettingsController::SchedulePostMountRefresh() {
  BrowserThread::PostTask(BrowserThreadId::UI, []() {
    auto& controller = SettingsController::Instance();
    controller.suppress_preset_apply_ = false;
    controller.DirtyAll();
  });
}

void SettingsController::OpenSettings() {
  LoadFromSession();
  ShellHost::Instance().PushTransient({.key = "settings", .rml_path = "views/settings.rml", .toolbar_label = "Settings"});
  ShellHost::Instance().DirtyWindow();
  SchedulePostMountRefresh();
}

void SettingsController::OnApplyLlmPreset(const std::string& preset) {
  if (suppress_preset_apply_) {
    return;
  }

  state_.llm_preset = preset;

  AppConfig scratch = SessionStore::Instance().Snapshot().config;
  ApplyPreset(scratch, preset, state_.llm_base_url.c_str());
  state_.llm_base_url = scratch.llm.base_url;
  if (preset == "ollama") {
    state_.llm_api_key = "";
    state_.llm_api_key_env = "";
  }

  auto& host = DataModelHost::Instance();
  host.Dirty("settings", "llm_preset");
  host.Dirty("settings", "llm_base_url");
  host.Dirty("settings", "llm_api_key");
  host.Dirty("settings", "llm_api_key_env");
}

AppConfig SettingsController::BuildConfigFromDraft() const {
  SettingsDraft draft;
  draft.llm_preset = state_.llm_preset.c_str();
  draft.llm_base_url = state_.llm_base_url.c_str();
  draft.llm_model = state_.llm_model.c_str();
  draft.llm_api_key = state_.llm_api_key.c_str();
  draft.llm_api_key_env = state_.llm_api_key_env.c_str();
  return ApplySettingsDraft(SessionStore::Instance().Snapshot().config, draft);
}

void SettingsController::OnSaveSettings() {
  const AppConfig config = BuildConfigFromDraft();
  const std::string preset = state_.llm_preset.c_str();

  if (auto saved = SessionStore::Instance().SaveConfig(config); !saved) {
    state_.status = saved.error().message;
    DataModelHost::Instance().Dirty("settings", "status");
    return;
  }

  ProfilePreferences profile_prefs = SessionStore::Instance().Snapshot().profile_prefs;
  profile_prefs.theme = state_.theme.c_str();
  if (auto saved = SessionStore::Instance().SaveProfilePrefs(profile_prefs); !saved) {
    state_.status = saved.error().message;
    DataModelHost::Instance().Dirty("settings", "status");
    return;
  }

  const AppConfig& persisted = SessionStore::Instance().Snapshot().config;
  state_.llm_base_url = persisted.llm.base_url;
  if (preset == "ollama") {
    state_.llm_api_key = "";
    state_.llm_api_key_env = "";
  }

  state_.status = "Settings saved";
  DataModelHost::Instance().Dirty("settings", "status");
  DataModelHost::Instance().Dirty("settings", "llm_base_url");
  DataModelHost::Instance().Dirty("settings", "llm_api_key");
  DataModelHost::Instance().Dirty("settings", "llm_api_key_env");
  ShellFeedback::ShowToast(ShellHost::Instance().State(), "Settings saved");
  ShellHost::Instance().DirtyWindow();
}

void SettingsController::OnResetDefaults() {
  SessionStore::Instance().Mutable().config = SessionStore::Instance().DefaultConfig();
  SessionStore::Instance().Mutable().profile_prefs = SessionStore::Instance().DefaultProfilePrefs();
  LoadFromSession();
  DirtyAll();
  state_.status = "Defaults loaded (save to persist)";
  DataModelHost::Instance().Dirty("settings", "status");
  SchedulePostMountRefresh();
}

void SettingsController::SaveSettingsCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                               const Rml::VariantList& /*args*/) {
  Instance().OnSaveSettings();
}

void SettingsController::ResetDefaultsCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                const Rml::VariantList& /*args*/) {
  Instance().OnResetDefaults();
}

void SettingsController::ApplyLlmPresetCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev,
                                                const Rml::VariantList& /*args*/) {
  auto& controller = Instance();
  const std::string preset =
      ev.GetParameter<Rml::String>("value", controller.state_.llm_preset).c_str();
  controller.OnApplyLlmPreset(preset);
}

void SettingsController::OpenSettingsCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                              const Rml::VariantList& /*args*/) {
  Instance().OpenSettings();
}

} // namespace pbr
