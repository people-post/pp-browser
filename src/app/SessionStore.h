#pragma once

#include "app/Bootstrap.h"
#include "common/Error.h"

#include <functional>
#include <string>
#include <vector>

namespace pbr {

class SessionStore {
public:
  static SessionStore& Instance();

  void Initialize(BootstrapResult bootstrap);
  bool IsInitialized() const { return initialized_; }

  const BootstrapResult& Snapshot() const;
  BootstrapResult& Mutable();

  AppConfig DefaultConfig() const;
  ProfilePreferences DefaultProfilePrefs() const;

  Roe<void> SaveConfig(const AppConfig& config);
  Roe<void> SaveProfilePrefs(const ProfilePreferences& prefs);
  Roe<void> ReloadConfig();

  void AddConfigListener(std::function<void(const AppConfig&)> listener);
  void AddThemeListener(std::function<void(const std::string& theme)> listener);

private:
  SessionStore() = default;

  void NotifyConfigListeners(const AppConfig& config);
  void NotifyThemeListeners(const std::string& theme);

  bool initialized_ = false;
  BootstrapResult bootstrap_;
  std::vector<std::function<void(const AppConfig&)>> config_listeners_;
  std::vector<std::function<void(const std::string&)>> theme_listeners_;
};

} // namespace pbr
