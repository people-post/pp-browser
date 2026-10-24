#pragma once

#include "feature/settings/SettingsUiState.h"
#include "foundation/data/SessionStore.h"
#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** What the settings screen's parts reach back for (SettingsController). UI thread. */
class SettingsHost {
public:
  virtual ~SettingsHost() = default;
  virtual SessionStore& Store() = 0;
  /** The failure in the status line (and a toast). */
  virtual void ReportFailure(const Error& err) = 0;
  /** The status line under the open section ("" clears it). */
  virtual void ShowStatus(const std::string& status) = 0;
  /** Settings read again from the session, every field redrawn (after a reset, a PIN change). */
  virtual void Resync(bool include_profile_nickname = true) = 0;
  /** What the settings show, as last taken from the view (PullFromView) or the session. */
  virtual SettingsUiState& UiState() = 0;
  /** The view's edits into UiState(). */
  virtual void PullFromView() = 0;
  /** UiState() into the view, every field redrawn. */
  virtual void PushToView(bool include_profile_nickname) = 0;
  /** A nickname edited in Me → Profile, saved (no toast). */
  virtual void SaveProfileNickname() = 0;
  /** After a reset that changes what the shell shows. */
  virtual void RequestSyncLayout(const char* reason) = 0;
  /** Saved tool permissions forgotten (Security), and shown so. */
  virtual Roe<void> ForgetToolPermissions() = 0;
};

} // namespace pbr
