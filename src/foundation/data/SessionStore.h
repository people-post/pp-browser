#pragma once

#include "foundation/data/BootstrapTypes.h"
#include "foundation/data/ConfigSecrets.h"
#include "common/Error.h"

#include <functional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class SessionStore {
public:
  void Initialize(BootstrapResult bootstrap);
  bool IsInitialized() const { return initialized_; }

  const BootstrapResult& Snapshot() const;
  BootstrapResult& Mutable();

  AppConfig DefaultConfig() const;
  ProfilePreferences DefaultProfilePrefs() const;

  /**
   * Typed API keys live here, not in config.json (PB-034). SaveConfig stores them through it and writes the
   * file without them; loads overlay them back. Until it is Ready() (vault unlocked) the file is written as
   * before, so nothing is lost; ApplyConfigSecrets moves such keys out once the vault opens.
   */
  void SetConfigSecrets(IConfigSecrets* secrets);
  /** After unlock: overlay the stored keys and migrate any typed key still in config.json into the store. */
  Roe<void> ApplyConfigSecrets();

  Roe<void> SaveConfig(const AppConfig& config);
  Roe<void> SaveProfilePrefs(const ProfilePreferences& prefs);
  Roe<void> ReloadConfig();
  Roe<void> ReloadProfilePrefs();
  Roe<void> ReloadFromDisk();

  void AddConfigListener(std::function<void(const AppConfig&)> listener);
  void AddProfilePrefsListener(std::function<void(const ProfilePreferences&)> listener);
  void AddThemeListener(std::function<void(const std::string& theme)> listener);
  void AddAppearanceListener(std::function<void(const std::string& appearance)> listener);
  void AddLanguageListener(std::function<void(const std::string& language)> listener);
  void AddChromeMaterialListener(
      std::function<void(bool reduce_transparency, bool compact_chrome_frost)> listener);

  SessionStore() = default;

private:

  void NotifyConfigListeners(const AppConfig& config);
  void NotifyProfilePrefsListeners(const ProfilePreferences& prefs);
  void NotifyThemeListeners(const std::string& theme);
  void NotifyAppearanceListeners(const std::string& appearance);
  void NotifyLanguageListeners(const std::string& language);
  void NotifyChromeMaterialListeners(bool reduce_transparency, bool compact_chrome_frost);
  void DiffAndNotifyProfilePrefs(const ProfilePreferences& previous, const ProfilePreferences& next);

  Roe<void> OverlayStoredSecrets(AppConfig& config) const;

  bool initialized_ = false;
  IConfigSecrets* config_secrets_ = nullptr;
  BootstrapResult bootstrap_;
  std::vector<std::function<void(const AppConfig&)>> config_listeners_;
  std::vector<std::function<void(const ProfilePreferences&)>> profile_prefs_listeners_;
  std::vector<std::function<void(const std::string&)>> theme_listeners_;
  std::vector<std::function<void(const std::string&)>> appearance_listeners_;
  std::vector<std::function<void(const std::string&)>> language_listeners_;
  std::vector<std::function<void(bool, bool)>> chrome_material_listeners_;
};

} // namespace pbr
