#pragma once

#include "feature/settings/SettingsSectionHandler.h"
#include "common/PbrCompat.h"

namespace pbr {

std::string AttachmentDownloadPolicyDisplayLabel(const std::string& policy);

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
