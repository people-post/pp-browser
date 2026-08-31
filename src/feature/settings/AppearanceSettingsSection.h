#pragma once

#include "feature/settings/SettingsCommands.h"
#include "feature/settings/SettingsSectionHandler.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** Display label for theme pref (`system` / `light` / `dark`). */
std::string ThemeDisplayLabel(const std::string& appearance_pref);

/** Me → Appearance. Language labels via SettingsCommands ports — no LocalizationService::Instance. */
class AppearanceSettingsSection final : public SettingsSectionHandler {
public:
  void BindPorts(SettingsCommands* commands);

  const char* Id() const override;
  SettingsSectionListItem ListItem() const override;
  SettingsFlushMode FlushMode() const override;

  void SyncFromSession(const BootstrapResult& bootstrap, SettingsUiState& state) override;
  bool IsPersisted(const SettingsUiState& state, const BootstrapResult& bootstrap) const override;
  Roe<void> Flush(SettingsUiState& state, SessionStore& store) override;
  void ResetToDefaults(SettingsUiState& state, const SessionStore& store) override;

private:
  std::string ResolveLanguageLabel(const std::string& language_pref) const;

  SettingsCommands* commands_ = nullptr;
};

} // namespace pbr
