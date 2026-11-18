#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pbr {

/**
 * Per-launch log file fed from the root logger. Lines are flushed as written, so the tail
 * survives a crash. Previous launches rotate to `pp-browser.1.log` (newest) … `pp-browser.N.log`;
 * rotated files are kept by age and total size (`LogRetention`), oldest deleted first; the newest is always kept.
 * Flags / location: docs/ops/CONFIGURATION.md § Log file.
 */

/** How long and how much rotated log to keep, per file family (`pp-browser`, `metrics`, `diag`). */
struct LogRetention {
  std::chrono::hours max_age{24 * 7};
  std::uintmax_t max_bytes = 50ull * 1024 * 1024;
};

/** One log file as seen by the retention rule. */
struct LogFileInfo {
  std::string name;
  std::filesystem::file_time_type mtime;
  std::uintmax_t size = 0;
  bool current = false; ///< the running launch's file: counts toward the size, is never deleted
};

class LogFile {
public:
  /**
   * Names to delete: every non-current file older than `max_age`, then the oldest non-current
   * files until the rest (current included) is within `max_bytes`. The newest non-current file (the
   * previous launch) is never deleted. Equal times keep input order (earlier = older). Pure: no file access.
   */
  static std::vector<std::string> ExpiredFiles(std::vector<LogFileInfo> files, std::filesystem::file_time_type now,
                                               const LogRetention& retention);

  /** `{data_dir}/logs/pp-browser.log` */
  static std::string DefaultPath(const std::string& data_dir);

  /** Rotated name for launch `index` back (1 = previous): `pp-browser.1.log`. */
  static std::string RotatedPath(const std::string& path, std::size_t index);

  /**
   * Shift every `.N` up by one and `path` → `.1`, then delete rotated files past `retention`
   * (age from the file's modification time). Missing files are skipped.
   */
  static void Rotate(const std::string& path, const LogRetention& retention = {});

  /**
   * Rotate, create `path` owner-only and attach it to the root logger. Returns the path in
   * use, or empty when the file could not be opened (reported on the root logger).
   */
  static std::string Install(const std::string& path, const LogRetention& retention = {});

  /** `metrics.log` next to the log file at `log_path`. */
  static std::string MetricsPath(const std::string& log_path);

  /**
   * Rotate and attach `MetricsPath(log_path)` to the "Metrics" logger (common/Metrics.h), so
   * operational metrics are written at any root log level and live in a file of their own. Returns
   * the path in use, or empty when the file could not be opened.
   */
  static std::string InstallMetrics(const std::string& log_path, const LogRetention& retention = {});

  /** `diag.log` next to the log file at `log_path`. */
  static std::string DiagPath(const std::string& log_path);

  /**
   * Rotate and attach `DiagPath(log_path)` to the "Diag" logger (common/Diag.h): the troubleshooting
   * events a user may send (docs/contracts/DIAGNOSTICS.md), written at any root log level.
   */
  static std::string InstallDiag(const std::string& log_path, const LogRetention& retention = {});
};

} // namespace pbr
