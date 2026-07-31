#pragma once

#include "base/data/BootstrapTypes.h"
#include "base/data/SessionStore.h"
#include "common/Error.h"
#include "feature/settings/SettingsUiState.h"

namespace pbr {

enum class SettingsFlushMode {
  Debounced,
  Immediate,
};

class SettingsSectionHandler {
public:
  virtual ~SettingsSectionHandler() = default;

  virtual const char* Id() const = 0;
  virtual SettingsSectionListItem ListItem() const = 0;
  virtual SettingsFlushMode FlushMode() const = 0;
  virtual bool IsWritable() const { return true; }

  virtual void SyncFromSession(const BootstrapResult& bootstrap, SettingsUiState& state) = 0;
  virtual bool IsPersisted(const SettingsUiState& state, const BootstrapResult& bootstrap) const = 0;
  virtual Roe<void> Flush(SettingsUiState& state, SessionStore& store) = 0;
  virtual void ResetToDefaults(SettingsUiState& state, const SessionStore& store) = 0;
};

} // namespace pbr
