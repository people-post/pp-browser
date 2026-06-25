#include "base/data/SessionStore.h"

#include "base/data/Config.h"
#include "base/data/UserPreferences.h"

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
  bootstrap_.profile_prefs = prefs;
  NotifyThemeListeners(prefs.theme);
  NotifyAppearanceListeners(prefs.appearance);
  return {};
}

Roe<void> SessionStore::ReloadConfig() {
  auto reloaded = Config::LoadFromFile(bootstrap_.config_path);
  if (!reloaded) {
    return reloaded.error();
  }
  bootstrap_.config = std::move(*reloaded);
  NotifyConfigListeners(bootstrap_.config);
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

} // namespace pbr
