#pragma once

#include "feature/settings/SettingsCommands.h"
#include "feature/settings/SettingsSectionHandler.h"
#include "common/PbrCompat.h"

#include <string>

namespace pbr {

/** Recompute letter + tone from nickname and stable ids already on `state`. */
void RefreshProfileAvatarGlyph(SettingsUiState& state);

/**
 * Display only. True when the card needs action, so the big blue register button is shown; false when the card is
 * fine and the plain "Update now" row is enough. `status_label` is the identity view's fixed English label
 * (RegistrationStatusLabel). Only "active" is fine; every other or unknown label needs action.
 */
bool ProfileRegisterIsPrimary(bool show_register, const std::string& status_label);

/** Me → Profile section. Identity I/O via SettingsCommands ports — no ConversationsHub. */
class ProfileSettingsSection : public SettingsSectionHandler {
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
  void ApplyIdentityView(const ProfileIdentityView& view, SettingsUiState& state) const;

  SettingsCommands* commands_ = nullptr;
};

} // namespace pbr
