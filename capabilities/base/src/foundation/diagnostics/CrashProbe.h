#pragma once

// THROWAWAY device probe (branch kenneth/crash-probe, never merged), compiled only with -DPP_CRASH_PROBE=ON.
// What does iOS hand an app after a crash / a hang, and when? (DEV/PLAN-crash-logs-from-system.md, step 1)

#include <functional>
#include <string>

namespace pbr {

class CrashProbe {
public:
  struct Status {
    int payloads = 0;      ///< payload files written so far
    std::string last_time; ///< arrival time of the newest one, "" if none
  };

  /** `{data_dir}/diagnostics/probe`, next to crash_pending.txt. */
  static std::string ProbeDir(const std::string& data_dir);

  /** iOS: subscribe to MetricKit and log one summary line per launch / payload. Elsewhere: only a log line. */
  static void Start(const std::string& data_dir);

  static Status ReadStatus(const std::string& data_dir);

  /** kind 0: null write (SIGSEGV); 1: uncaught C++ exception on a helper thread (std::terminate -> SIGABRT). */
  [[noreturn]] static void Crash(int kind);
  /** PROBE ONLY: fake "new version" card. 0 = real client-compat advice, 1 = version 9.9.9 with a usable link,
   *  2 = version 9.9.9 with no link. UI thread only. */
  static int UpdateCardMode();
  static int CycleUpdateCardMode();
  static void SetUpdateCardRefresh(std::function<void()> refresh);
  /** Blocks the calling (UI) thread. */
  static void Hang(int seconds);
};

} // namespace pbr
