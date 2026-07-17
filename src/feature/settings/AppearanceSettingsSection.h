#pragma once

#include "feature/settings/SettingsSectionHandler.h"

#include <string>

namespace pbr {

/** Display label for theme pref (`system` / `light` / `dark`). */
std::string ThemeDisplayLabel(const std::string& appearance_pref);

class AppearanceSettingsSection final : public SettingsSectionHandler {
public:
  const char* Id() const override;
  SettingsSectionListItem ListItem() const override;
  SettingsFlushMode FlushMode() const override;

  void SyncFromSession(const BootstrapResult& bootstrap, SettingsUiState& state) override;
  bool IsPersisted(const SettingsUiState& state, const BootstrapResult& bootstrap) const override;
  Roe<void> Flush(SettingsUiState& state, SessionStore& store) override;
  void ResetToDefaults(SettingsUiState& state, const SessionStore& store) override;
};

} // namespace pbr
