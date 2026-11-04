#pragma once

#include "feature/settings/SettingsSectionHandler.h"
#include "common/PbrCompat.h"
#include "foundation/platform/Platform.h"

namespace pbr {

std::string AttachmentDownloadPolicyDisplayLabel(const std::string& policy);

/** The Wi-Fi / mobile-network ticks show only on a phone and only while "everything" is chosen. */
bool AttachmentAutoTogglesVisible(const SettingsUiState& state, PlatformKind platform);
/** Picks a policy; choosing "everything" starts with Wi-Fi ticked and the mobile network not. */
void ChooseAttachmentDownloadPolicy(SettingsUiState& state, const std::string& policy);
/** Flips one tick; unticking the last one falls back to the default policy. */
void ToggleAttachmentAutoNetwork(SettingsUiState& state, bool cellular);

class StorageSettingsSection final : public SettingsSectionHandler {
public:
  const char* Id() const override;
  SettingsSectionListItem ListItem() const override;
  SettingsFlushMode FlushMode() const override;
  bool IsWritable() const override;

  void SyncFromSession(const BootstrapResult& bootstrap, SettingsUiState& state) override;
  bool IsPersisted(const SettingsUiState& state, const BootstrapResult& bootstrap) const override;
  Roe<void> Flush(SettingsUiState& state, SessionStore& store) override;
  void ResetToDefaults(SettingsUiState& state, const SessionStore& store) override;
};

} // namespace pbr
