#pragma once

#include "feature/settings/SettingsCommands.h"
#include "feature/settings/SettingsSectionHandler.h"

#include <string>

namespace pbr {

std::string GroupInvitePolicyDisplayLabel(const std::string& policy);

class SecuritySettingsSection final : public SettingsSectionHandler {
public:
  void BindPorts(SettingsCommands* commands);

  const char* Id() const override;
  SettingsSectionListItem ListItem() const override;
  SettingsFlushMode FlushMode() const override;
  bool IsWritable() const override { return true; }

  void SyncFromSession(const BootstrapResult& bootstrap, SettingsUiState& state) override;
  bool IsPersisted(const SettingsUiState& state, const BootstrapResult& bootstrap) const override;
  Roe<void> Flush(SettingsUiState& state, SessionStore& store) override;
  void ResetToDefaults(SettingsUiState& state, const SessionStore& store) override;

  /** Clears remembered Always/Never tool decisions back to Ask defaults. */
  Roe<void> ResetToolPermissions(SettingsUiState& state, SessionStore& store);

private:
  SettingsCommands* commands_ = nullptr;
};

} // namespace pbr
