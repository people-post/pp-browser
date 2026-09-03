#pragma once

#include "feature/settings/SettingsCommands.h"
#include "feature/settings/SettingsSectionHandler.h"
#include "common/PbrCompat.h"

namespace pbr {

/** Recompute letter + tone from nickname and stable ids already on `state`. */
void RefreshProfileAvatarGlyph(SettingsUiState& state);

/** Me → Profile section. Identity I/O via SettingsCommands ports — no MessagingHub. */
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
