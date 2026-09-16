#pragma once

#include <string>

namespace pbr {

/**
 * Bare-minimal crash capture: write `{data_dir}/diagnostics/crash_pending.txt`
 * from signal / terminate handlers, then optionally upload on next launch.
 */
class CrashDump {
public:
  /** Resolve dump path under the process data directory. */
  static std::string PendingPath(const std::string& data_dir);

  /**
   * Remember dump directory, install signal / terminate handlers, and feed
   * CrashBreadcrumbs. Safe to call once early in main (after DataDir is known).
   */
  static void Install(const std::string& data_dir);

  /** True when a pending dump file exists. */
  static bool HasPending(const std::string& data_dir);

  /** Read pending dump body; empty if missing. */
  static std::string ReadPending(const std::string& data_dir);

  /** Delete pending dump after upload or discard. */
  static void ClearPending(const std::string& data_dir);

  /**
   * Write a rich dump from a non-signal context (uncaught exception, tests).
   * Overwrites any existing pending file.
   */
  static void WritePending(const std::string& data_dir, const std::string& reason);
};

} // namespace pbr
