#pragma once

#include "feature/settings/SettingsSectionHandler.h"

namespace pbr {

class MessagingHub;

class ProfileSettingsSection : public SettingsSectionHandler {
public:
  void BindMessaging(MessagingHub& messaging);
  MessagingHub& Hub();
  const MessagingHub& Hub() const;
  const char* Id() const override;
  SettingsSectionListItem ListItem() const override;
  SettingsFlushMode FlushMode() const override;

  void SyncFromSession(const BootstrapResult& bootstrap, SettingsUiState& state) override;
  bool IsPersisted(const SettingsUiState& state, const BootstrapResult& bootstrap) const override;
  Roe<void> Flush(SettingsUiState& state, SessionStore& store) override;
  void ResetToDefaults(SettingsUiState& state, const SessionStore& store) override;

  static Roe<void> RegisterIdentity(SettingsUiState& state, MessagingHub& messaging);
  static Roe<void> RotateBriefLlmKey(SettingsUiState& state, MessagingHub& messaging);
  MessagingHub* messaging_ = nullptr;

};

} // namespace pbr
