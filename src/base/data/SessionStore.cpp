#include "base/data/SessionStore.h"

#include "base/data/Config.h"
#include "base/data/UserPreferences.h"

#include <filesystem>

namespace pbr {

SessionStore& SessionStore::Instance() {
  static SessionStore store;
  return store;
}

void SessionStore::Initialize(BootstrapResult bootstrap) {
  bootstrap_ = std::move(bootstrap);
  initialized_ = true;
}

const BootstrapResult& SessionStore::Snapshot() const {
  return bootstrap_;
}

BootstrapResult& SessionStore::Mutable() {
  return bootstrap_;
}

AppConfig SessionStore::DefaultConfig() const {
  return Config::DefaultAppConfig();
}

ProfilePreferences SessionStore::DefaultProfilePrefs() const {
  return UserPreferences::DefaultProfile();
}

Roe<void> SessionStore::SaveConfig(const AppConfig& config) {
  const std::string& path = bootstrap_.config_path;
  if (auto saved = Config::SaveToFile(path, config); !saved) {
    return saved.error();
  }

  auto reloaded = Config::LoadFromFile(path);
  if (!reloaded) {
    return reloaded.error();
  }

  bootstrap_.config = std::move(*reloaded);
  NotifyConfigListeners(bootstrap_.config);
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

  const std::string previous_theme = bootstrap_.profile_prefs.theme;
  const std::string previous_appearance = bootstrap_.profile_prefs.appearance;
  const std::string previous_language = bootstrap_.profile_prefs.language;
  bootstrap_.profile_prefs = std::move(*reloaded);

  if (bootstrap_.profile_prefs.theme != previous_theme) {
    NotifyThemeListeners(bootstrap_.profile_prefs.theme);
  }
  if (bootstrap_.profile_prefs.appearance != previous_appearance) {
    NotifyAppearanceListeners(bootstrap_.profile_prefs.appearance);
  }
  if (bootstrap_.profile_prefs.language != previous_language) {
    NotifyLanguageListeners(bootstrap_.profile_prefs.language);
  }
  return {};
}

Roe<void> SessionStore::ReloadConfig() {
  // Match Config::Load / bootstrap: missing config is not an error — use defaults.
  if (bootstrap_.config_path.empty() || !std::filesystem::exists(bootstrap_.config_path)) {
    bootstrap_.config = Config::DefaultAppConfig();
    NotifyConfigListeners(bootstrap_.config);
    return {};
  }

  auto reloaded = Config::LoadFromFile(bootstrap_.config_path);
  if (!reloaded) {
    return reloaded.error();
  }
  bootstrap_.config = std::move(*reloaded);
  NotifyConfigListeners(bootstrap_.config);
  return {};
}

Roe<void> SessionStore::ReloadProfilePrefs() {
  auto reloaded = UserPreferences::LoadProfile(bootstrap_.profile_data_dir);
  if (!reloaded) {
    return reloaded.error();
  }

  const std::string previous_theme = bootstrap_.profile_prefs.theme;
  const std::string previous_appearance = bootstrap_.profile_prefs.appearance;
  const std::string previous_language = bootstrap_.profile_prefs.language;
  bootstrap_.profile_prefs = std::move(*reloaded);

  if (bootstrap_.profile_prefs.theme != previous_theme) {
    NotifyThemeListeners(bootstrap_.profile_prefs.theme);
  }
  if (bootstrap_.profile_prefs.appearance != previous_appearance) {
    NotifyAppearanceListeners(bootstrap_.profile_prefs.appearance);
  }
  if (bootstrap_.profile_prefs.language != previous_language) {
    NotifyLanguageListeners(bootstrap_.profile_prefs.language);
  }
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

void SessionStore::AddThemeListener(std::function<void(const std::string& theme)> listener) {
  if (listener) {
    theme_listeners_.push_back(std::move(listener));
  }
}

void SessionStore::NotifyConfigListeners(const AppConfig& config) {
  for (const auto& listener : config_listeners_) {
    listener(config);
  }
}

void SessionStore::NotifyThemeListeners(const std::string& theme) {
  for (const auto& listener : theme_listeners_) {
    listener(theme);
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

} // namespace pbr
