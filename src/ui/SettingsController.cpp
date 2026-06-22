#include "ui/SettingsController.h"

#include "app/AppPaths.h"
#include "app/Config.h"
#include "app/UserPreferences.h"
#include "demo/ChatDemo.h"
#include "platform/PlatformDefaults.h"
#include "platform/Platform.h"
#include "ui/DataModelHost.h"
#include "ui/ShellFeedback.h"
#include "ui/ShellHost.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>

namespace pbr {

namespace {

std::string DetectPreset(const AppConfig& config) {
  if (config.llm.base_url.find("11434") != std::string::npos) {
    return "ollama";
  }
  if (config.llm.base_url == "https://api.openai.com/v1") {
    return "cloud";
  }
  return "custom";
}

} // namespace

SettingsController::SettingsController() {
  redirectLogger("SettingsController");
}

SettingsController& SettingsController::Instance() {
  static SettingsController controller;
  return controller;
}

void SettingsController::BindBootstrap(BootstrapResult bootstrap) {
  bootstrap_ = std::move(bootstrap);
  LoadFromBootstrap();
}

void SettingsController::LoadFromBootstrap() {
  const AppConfig& config = bootstrap_.config;
  state_.llm_preset = DetectPreset(config).c_str();
  state_.llm_base_url = config.llm.base_url.c_str();
  state_.llm_model = config.llm.model.c_str();
  state_.llm_api_key = config.llm.api_key.c_str();
  state_.llm_api_key_env = config.llm_api_key_env.c_str();
  state_.theme = bootstrap_.profile_prefs.theme.c_str();
  state_.profile_label = bootstrap_.profile_registry.ActiveProfileId().c_str();
  state_.config_dir = AppPaths::ConfigDir().c_str();
  state_.data_dir = bootstrap_.data_dir.c_str();
  state_.profile_dir = bootstrap_.profile_data_dir.c_str();
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

void SettingsController::OpenSettings() {
  LoadFromBootstrap();
  ShellHost::Instance().PushTransient({.key = "settings", .rml_path = "views/settings.rml", .toolbar_label = "Settings"});
  ShellHost::Instance().DirtyWindow();
  DirtyAll();
}

void SettingsController::OnSaveSettings() {
  AppConfig config = bootstrap_.config;
  const std::string preset = state_.llm_preset.c_str();

  if (preset == "ollama") {
    config.llm.base_url = "http://localhost:11434/v1";
    config.llm.require_api_key = false;
    config.llm.api_key.clear();
    config.llm_api_key_env.clear();
  } else if (preset == "cloud") {
    const AppConfig defaults = PlatformDefaults::For(Platform::Detect());
    config.llm.base_url = defaults.llm.base_url;
    config.llm.require_api_key = true;
  } else {
    config.llm.base_url = state_.llm_base_url.c_str();
    config.llm.require_api_key = true;
  }

  config.llm.model = state_.llm_model.c_str();

  const std::string inline_key = state_.llm_api_key.c_str();
  if (!inline_key.empty()) {
    config.llm.api_key = inline_key;
    config.llm_api_key_env.clear();
  } else {
    config.llm.api_key.clear();
    config.llm_api_key_env = state_.llm_api_key_env.c_str();
    if (!config.llm_api_key_env.empty()) {
      config.llm.require_api_key = true;
    }
  }

  if (preset == "ollama") {
    config.llm.require_api_key = false;
  }

  config.theme = state_.theme.c_str();

  const std::string config_path = bootstrap_.config_path.empty() ? AppPaths::ConfigFilePath() : bootstrap_.config_path;
  if (auto saved = Config::SaveToFile(config_path, config); !saved) {
    state_.status = saved.error().message.c_str();
    DataModelHost::Instance().Dirty("settings", "status");
    return;
  }

  bootstrap_.config = config;

  ProfilePreferences profile_prefs = bootstrap_.profile_prefs;
  profile_prefs.theme = state_.theme.c_str();
  if (auto saved = UserPreferences::SaveProfile(bootstrap_.profile_data_dir, profile_prefs); !saved) {
    state_.status = saved.error().message.c_str();
    DataModelHost::Instance().Dirty("settings", "status");
    return;
  }
  bootstrap_.profile_prefs = profile_prefs;

  ChatDemo::Instance().ApplyConfig(config);

  state_.status = "Settings saved";
  DataModelHost::Instance().Dirty("settings", "status");
  ShellFeedback::ShowToast(ShellHost::Instance().State(), "Settings saved");
  ShellHost::Instance().DirtyWindow();
}

void SettingsController::OnResetDefaults() {
  bootstrap_.config = Config::DefaultAppConfig();
  bootstrap_.profile_prefs = UserPreferences::DefaultProfile();
  LoadFromBootstrap();
  DirtyAll();
  state_.status = "Defaults loaded (save to persist)";
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

void SettingsController::OpenSettingsCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                              const Rml::VariantList& /*args*/) {
  Instance().OpenSettings();
}

} // namespace pbr
