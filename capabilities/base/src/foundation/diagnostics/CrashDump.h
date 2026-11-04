#pragma once

#include <string>

namespace pbr {

/**
 * Bare-minimal crash capture: write `{data_dir}/diagnostics/crash_pending.txt`
 * from signal / terminate handlers, then optionally upload on next launch.
 */
/** Who crashed, as the dump says: the app's slug and release version (the app hands them in). */
struct CrashIdentity {
  std::string product;
  std::string version;
};

class CrashDump {
public:
  /** "android" / "windows" / "ios" / "macos" / "linux" — the same label the dump carries. */
  static const char* OsLabel();

  /** Resolve dump path under the process data directory. */
  static std::string PendingPath(const std::string& data_dir);

  /**
   * Remember dump directory, install signal / terminate handlers, and feed
   * CrashBreadcrumbs. Safe to call once early in main (after DataDir is known). `identity` goes into every
   * dump (kept in fixed buffers: the signal handler reads them).
   */
  static void Install(const std::string& data_dir, const CrashIdentity& identity);

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

  /**
   * Write `backtrace_frames=<n>` plus one line per frame of the calling thread to `fd`, using
   * only stack buffers and write(). For the crash path and tests. Android lines read
   * `frame <i> pc= base= off= module= sym=` (feed `off` to llvm-symbolizer with the unstripped
   * .so); macOS / glibc print raw addresses; other platforms write nothing.
   */
  static void WriteNativeBacktrace(int fd);
};

} // namespace pbr
