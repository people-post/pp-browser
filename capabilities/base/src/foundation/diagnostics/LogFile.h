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
 * installing only rotates. `Prune` / `PruneAll` delete rotated files past a `LogRetention` (by age and total
 * size, oldest first; the newest is always kept), once the caller knows which retention applies
 * (`EffectiveRetention`: the device's "Collect run logs" switch). Flags / location: docs/ops/CONFIGURATION.md § Log file.
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

  /** Shift every `.N` up by one and `path` → `.1`. Missing files are skipped; nothing is deleted. */
  static void Rotate(const std::string& path);

  /**
   * `path` (when present, marked current) and every rotated `.N` file of its family, with their full paths, times
   * and sizes.
   */
  static std::vector<LogFileInfo> FamilyFiles(const std::string& path);

  /** Deletes the files of `path`'s family past `retention` (ExpiredFiles; age from the modification time). */
  static void Prune(const std::string& path, const LogRetention& retention);

  /** Prune for each family in the log directory: `log_path`, its metrics file and its diag file. */
  static void PruneAll(const std::string& log_path, const LogRetention& retention);

  /**
   * The one retention every log rule uses (pruning, packaging, "can send"): the default 7 days / 50 MB when
   * `collect` (the "Collect run logs" switch is on and allowed); otherwise max_age 0, which keeps only the current
   * file and the newest rotated one: this launch and the one before.
   */
  static LogRetention EffectiveRetention(bool collect);

  /**
   * Rotate, create `path` owner-only and attach it to the root logger. Returns the path in
   * use, or empty when the file could not be opened (reported on the root logger).
   */
  static std::string Install(const std::string& path);

  /** `metrics.log` next to the log file at `log_path`. */
  static std::string MetricsPath(const std::string& log_path);

  /**
   * Rotate and attach `MetricsPath(log_path)` to the "Metrics" logger (common/Metrics.h), so
   * operational metrics are written at any root log level and live in a file of their own. Returns
   * the path in use, or empty when the file could not be opened.
   */
  static std::string InstallMetrics(const std::string& log_path);

  /** `diag.log` next to the log file at `log_path`. */
  static std::string DiagPath(const std::string& log_path);

  /**
   * Rotate and attach `DiagPath(log_path)` to the "Diag" logger (common/Diag.h): the troubleshooting
   * events a user may send (docs/contracts/DIAGNOSTICS.md), written at any root log level.
   */
  static std::string InstallDiag(const std::string& log_path);
};

} // namespace pbr
