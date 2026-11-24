#include "foundation/data/SessionStore.h"

#include "foundation/data/Config.h"
#include "foundation/data/ConfigJson.h"
#include "foundation/data/ProfileServices.h"
#include "foundation/data/UiDemoConfig.h"
#include "foundation/data/UserPreferences.h"

#include <filesystem>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

/** What a reload compares: the file's keys, and the profile's promoted MCP, which config.json no longer holds. */
bool SameRunningConfig(const AppConfig& a, const AppConfig& b) {
  return ConfigToObject(a, Config::kConfigVersion) == ConfigToObject(b, Config::kConfigVersion) &&
         McpConfigToObject(a.promoted_mcp) == McpConfigToObject(b.promoted_mcp) &&
         a.promoted_mcp.user_set == b.promoted_mcp.user_set;
}

} // namespace

void SessionStore::Initialize(BootstrapResult bootstrap) {
  bootstrap_ = std::move(bootstrap);
  initialized_ = true;
  // The user's promoted MCP is the profile's (services.json, profile-model PM011): one config.json still holds (a
  // marked entry from before) moves there once; config.json drops it at its next save.
  ProfileServices services = LoadProfileServices(bootstrap_.profile_data_dir);
  if (!services.promoted_mcp && bootstrap_.config.promoted_mcp.user_set && !bootstrap_.profile_data_dir.empty()) {
    services.promoted_mcp = bootstrap_.config.promoted_mcp;
    (void)SaveProfileServices(bootstrap_.profile_data_dir, services);
  }
  OverlayProfileServices(bootstrap_.config, services);
}

Roe<void> SessionStore::KeepProfileServices(const AppConfig& config) const {
  if (bootstrap_.profile_data_dir.empty()) {
    return {};
  }
  ProfileServices services = LoadProfileServices(bootstrap_.profile_data_dir);
  if (config.promoted_mcp.user_set && (config.promoted_mcp.IsConfigured() || !config.promoted_mcp.enabled)) {
    services.promoted_mcp = config.promoted_mcp;
  } else {
    services.promoted_mcp.reset(); // cleared in Integrations: the backend's seed again
  }
  return SaveProfileServices(bootstrap_.profile_data_dir, services);
}

const BootstrapResult& SessionStore::Snapshot() const {
  return bootstrap_;
}

BootstrapResult& SessionStore::Mutable() {
  return bootstrap_;
}

AppConfig SessionStore::DefaultConfig() const {
  return Config::DefaultAppConfig(ui_demo_);
}

ProfilePreferences SessionStore::DefaultProfilePrefs() const {
  return UserPreferences::DefaultProfile();
}

void SessionStore::SetConfigSecrets(IConfigSecrets* secrets) {
  config_secrets_ = secrets;
}

Roe<void> SessionStore::OverlayStoredSecrets(AppConfig& config) const {
  if (!config_secrets_ || !config_secrets_->Ready()) {
    return {};
  }
  auto stored = config_secrets_->Load();
  if (!stored) {
    return stored.error();
  }
  OverlaySecrets(config, *stored);
  return {};
}

Roe<void> SessionStore::SaveConfig(const AppConfig& config) {
  const std::string& path = bootstrap_.config_path;
  const ConfigSecrets typed = TypedSecretsOf(config);
  if (config_secrets_ && config_secrets_->Ready()) {
    if (auto stored = config_secrets_->Store(typed); !stored) {
      return stored.error();
    }
  } else if (!typed.Empty()) {
    return Error("Unlock the profile before saving an API key");
  }
  if (auto kept = KeepProfileServices(config); !kept) {
    return kept.error();
  }
  // ConfigToObject never writes a key, so the file is safe to write as is.
  if (auto saved = Config::SaveToFile(path, config); !saved) {
    return saved.error();
  }

  auto reloaded = Config::LoadFromFile(path, ui_demo_);
  if (!reloaded) {
    return reloaded.error();
  }
  if (auto overlaid = OverlayStoredSecrets(*reloaded); !overlaid) {
    return overlaid.error();
  }
  OverlayProfileServices(*reloaded, LoadProfileServices(bootstrap_.profile_data_dir));

  bootstrap_.config = std::move(*reloaded);
  NotifyConfigListeners(bootstrap_.config);
  return {};
}

Roe<void> SessionStore::ApplyConfigSecrets() {
  if (!config_secrets_ || !config_secrets_->Ready()) {
    return {};
  }
  auto stored = config_secrets_->Load();
  if (!stored) {
    return stored.error();
  }
  AppConfig overlaid = bootstrap_.config;
  OverlaySecrets(overlaid, *stored);
  const bool changed = overlaid.llm.api_key != bootstrap_.config.llm.api_key;
  bootstrap_.config = std::move(overlaid);
  if (changed) {
    NotifyConfigListeners(bootstrap_.config);
  }
  return {};
}

Roe<void> SessionStore::SaveProfilePrefs(const ProfilePreferences& prefs) {
  if (auto saved = UserPreferences::SaveProfile(bootstrap_.profile_data_dir, prefs); !saved) {
    return saved.error();
  }

  auto reloaded = UserPreferences::LoadProfile(bootstrap_.profile_data_dir);
  if (!reloaded) {
    return reloaded.error();
  }

  if (ui_demo_) {
    ApplyUiDemoProfilePrefs(*reloaded);
  }
  const ProfilePreferences previous = bootstrap_.profile_prefs;
  bootstrap_.profile_prefs = std::move(*reloaded);
  DiffAndNotifyProfilePrefs(previous, bootstrap_.profile_prefs);
  return {};
}

Roe<void> SessionStore::ReloadConfig() {
  // Match Config::Load / bootstrap: missing config is not an error — use defaults.
  if (bootstrap_.config_path.empty() || !std::filesystem::exists(bootstrap_.config_path)) {
    AppConfig defaults = Config::DefaultAppConfig(ui_demo_);
    OverlayProfileServices(defaults, LoadProfileServices(bootstrap_.profile_data_dir));
    const bool changed = !SameRunningConfig(bootstrap_.config, defaults);
    bootstrap_.config = std::move(defaults);
    if (changed) {
      NotifyConfigListeners(bootstrap_.config);
    }
    return {};
  }

  auto reloaded = Config::LoadFromFile(bootstrap_.config_path, ui_demo_);
  if (!reloaded) {
    return reloaded.error();
  }
  if (auto overlaid = OverlayStoredSecrets(*reloaded); !overlaid) {
    return overlaid.error();
  }
  OverlayProfileServices(*reloaded, LoadProfileServices(bootstrap_.profile_data_dir));
  // Me tab opens ReloadFromDisk every time; skip listener churn when nothing changed so
  // ChatController::ApplyRuntimeConfig does not rebuild the agent (Cancel/find-someone race).
  const bool changed = !SameRunningConfig(bootstrap_.config, *reloaded);
  bootstrap_.config = std::move(*reloaded);
  if (changed) {
    NotifyConfigListeners(bootstrap_.config);
  }
  return {};
}

Roe<void> SessionStore::ReloadProfilePrefs() {
  auto reloaded = UserPreferences::LoadProfile(bootstrap_.profile_data_dir);
  if (!reloaded) {
    return reloaded.error();
  }

  if (ui_demo_) {
    ApplyUiDemoProfilePrefs(*reloaded);
  }
  const ProfilePreferences previous = bootstrap_.profile_prefs;
  bootstrap_.profile_prefs = std::move(*reloaded);
  DiffAndNotifyProfilePrefs(previous, bootstrap_.profile_prefs);
  return {};
}

Roe<void> SessionStore::ReloadFromDisk() {
  if (auto reloaded = ReloadConfig(); !reloaded) {
    return reloaded.error();
  }
  if (auto reloaded = ReloadProfilePrefs(); !reloaded) {
    return reloaded.error();
  }
  return {};
}

void SessionStore::AddConfigListener(std::function<void(const AppConfig&)> listener) {
  if (listener) {
    config_listeners_.push_back(std::move(listener));
  }
}

void SessionStore::AddProfilePrefsListener(std::function<void(const ProfilePreferences&)> listener) {
  if (listener) {
    profile_prefs_listeners_.push_back(std::move(listener));
  }
}

void SessionStore::NotifyConfigListeners(const AppConfig& config) {
  for (const auto& listener : config_listeners_) {
    listener(config);
  }
}

void SessionStore::NotifyProfilePrefsListeners(const ProfilePreferences& prefs) {
  for (const auto& listener : profile_prefs_listeners_) {
    listener(prefs);
  }
}

void SessionStore::DiffAndNotifyProfilePrefs(const ProfilePreferences& previous, const ProfilePreferences& next) {
  if (ProfilePrefsToObject(previous) == ProfilePrefsToObject(next)) {
    return;
  }
  NotifyProfilePrefsListeners(next);
  if (next.appearance != previous.appearance) {
    NotifyAppearanceListeners(next.appearance);
  }
  if (next.language != previous.language) {
    NotifyLanguageListeners(next.language);
  }
  if (next.reduce_transparency != previous.reduce_transparency ||
      next.compact_chrome_frost != previous.compact_chrome_frost) {
    NotifyChromeMaterialListeners(next.reduce_transparency, next.compact_chrome_frost);
  }
}

void SessionStore::AddAppearanceListener(std::function<void(const std::string& appearance)> listener) {
  if (listener) {
    appearance_listeners_.push_back(std::move(listener));
  }
}

void SessionStore::NotifyAppearanceListeners(const std::string& appearance) {
  for (const auto& listener : appearance_listeners_) {
    listener(appearance);
  }
}

void SessionStore::AddLanguageListener(std::function<void(const std::string& language)> listener) {
  if (listener) {
    language_listeners_.push_back(std::move(listener));
  }
}

void SessionStore::NotifyLanguageListeners(const std::string& language) {
  for (const auto& listener : language_listeners_) {
    listener(language);
  }
}

void SessionStore::AddChromeMaterialListener(
    std::function<void(bool reduce_transparency, bool compact_chrome_frost)> listener) {
  if (listener) {
    chrome_material_listeners_.push_back(std::move(listener));
  }
}

void SessionStore::NotifyChromeMaterialListeners(bool reduce_transparency, bool compact_chrome_frost) {
  for (const auto& listener : chrome_material_listeners_) {
    listener(reduce_transparency, compact_chrome_frost);
  }
}

} // namespace pbr
