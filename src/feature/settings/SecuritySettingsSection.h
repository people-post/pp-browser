#pragma once

#include "feature/settings/SettingsSectionHandler.h"

namespace pbr {

class SecuritySettingsSection final : public SettingsSectionHandler {
public:
  const char* Id() const override;
  SettingsSectionListItem ListItem() const override;
  SettingsFlushMode FlushMode() const override;
  bool IsWritable() const override { return false; }

  void SyncFromSession(const BootstrapResult& bootstrap, SettingsUiState& state) override;
  bool IsPersisted(const SettingsUiState& state, const BootstrapResult& bootstrap) const override;
  Roe<void> Flush(SettingsUiState& state, SessionStore& store) override;
  void ResetToDefaults(SettingsUiState& state, const SessionStore& store) override;
};

} // namespace pbr
