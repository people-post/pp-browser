#pragma once

#include "feature/settings/SettingsCommands.h"
#include "feature/settings/SettingsSectionHandler.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

std::string GroupInvitePolicyDisplayLabel(const std::string& policy);
std::string CallInvitePolicyDisplayLabel(const std::string& policy);
/** "Every time" / "5 minutes" / "15 minutes" / "1 hour" for the PIN grace window (#21). */
std::string UnlockGraceDisplayLabel(int minutes);
/** "Unlock with Face ID" / "… Touch ID" / "… fingerprint" for the sensor `kind` (face / touch / fingerprint). */
std::string BiometricSwitchLabel(const std::string& kind);

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
