#pragma once

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
  virtual void Resync() = 0;
  /** After a reset that changes what the shell shows. */
  virtual void RequestSyncLayout(const char* reason) = 0;
  /** Saved tool permissions forgotten (Security), and shown so. */
  virtual Roe<void> ForgetToolPermissions() = 0;
};

} // namespace pbr
