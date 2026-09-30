#pragma once

#include <cstddef>
#include <string>

namespace pbr {

/**
 * Per-launch log file fed from the root logger. Lines are flushed as written, so the tail
 * survives a crash. Previous launches rotate to `pp-browser.1.log` … `pp-browser.N.log`.
 * Flags / location: docs/ops/CONFIGURATION.md § Log file.
 */
class LogFile {
public:
  static constexpr std::size_t kDefaultKeep = 5;

  /** `{data_dir}/logs/pp-browser.log` */
  static std::string DefaultPath(const std::string& data_dir);

  /** Rotated name for launch `index` back (1 = previous): `pp-browser.1.log`. */
  static std::string RotatedPath(const std::string& path, std::size_t index);

  /** Shift `path` → `.1` → … → `.keep`, dropping older files. Missing files are skipped. */
  static void Rotate(const std::string& path, std::size_t keep);

  /**
   * Rotate, create `path` owner-only and attach it to the root logger. Returns the path in
   * use, or empty when the file could not be opened (reported on the root logger).
   */
  static std::string Install(const std::string& path, std::size_t keep = kDefaultKeep);

  /** `metrics.log` next to the log file at `log_path`. */
  static std::string MetricsPath(const std::string& log_path);

  /**
   * Rotate and attach `MetricsPath(log_path)` to the "Metrics" logger (common/Metrics.h), so
   * operational metrics are written at any root log level and live in a file of their own —
   * the only one a collector should read (no account ids, PeerIds or addresses). Returns the path
   * in use, or empty when the file could not be opened.
   */
  static std::string InstallMetrics(const std::string& log_path, std::size_t keep = kDefaultKeep);
};

} // namespace pbr
