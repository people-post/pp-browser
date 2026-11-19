#pragma once

#include "common/Error.h"
#include "foundation/diagnostics/LogFile.h"
#include "common/PbrCompat.h"

#include <optional>
#include <string>

namespace pbr {

/**
 * The device-wide "Collect run logs" switch (MachinePreferences::collect_run_logs, `{data_dir}/machine.json`) as every
 * log rule reads it (DEV/PLAN-send-run-logs.md). `allowed` is the feature's gate (RunLogsAllowed): when false the
 * switch counts as off whatever is stored.
 */
class RunLogPrefs {
public:
  /** The switch: false when not allowed; nothing when machine.json cannot be read (callers treat that as off). */
  static std::optional<bool> ReadCollect(const std::string& data_dir, bool allowed);

  /** The retention the switch gives: unknown counts as off (only this launch and the one before). */
  static LogRetention Retention(const std::string& data_dir, bool allowed);

  /**
   * Startup, once the log files are installed: prunes the log directory of `log_path` with the switch's retention.
   * When the switch cannot be read nothing is deleted this launch (a lost setting must not cost 7 days of logs);
   * when not allowed the switch is not read and the logs go down to two launches. Returns whether it pruned.
   */
  static bool PruneAtStartup(const std::string& data_dir, const std::string& log_path, bool allowed);

  /**
   * The Settings switch: stores `on` in machine.json (refused, nothing written, when the file cannot be read: its
   * other settings would be lost). Turning it off prunes `log_path`'s directory to two launches at once; on deletes
   * nothing.
   */
  static Roe<void> SetCollect(const std::string& data_dir, const std::string& log_path, bool on);
};

} // namespace pbr
