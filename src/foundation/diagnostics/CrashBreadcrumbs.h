#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace pbr {

/**
 * Fixed-size ring of recent log lines for crash dumps.
 * Append is best-effort; signal handlers may read a slightly torn snapshot.
 */
class CrashBreadcrumbs {
public:
  static constexpr std::size_t kCapacity = 80;
  static constexpr std::size_t kLineBytes = 240;

  /** Install a root Logger handler that feeds the ring (idempotent). */
  static void InstallLoggerHandler();

  static void Append(const std::string& line);

  /** Oldest → newest snapshot for non-signal dump writers. */
  static std::vector<std::string> Snapshot();

  /**
   * Signal-safe dump of the ring into `out` (NUL-terminated, truncated).
   * Returns bytes written excluding the trailing NUL.
   */
  static std::size_t CopySignalSafe(char* out, std::size_t out_bytes);
};

} // namespace pbr
