#pragma once

#include "common/Logger.h"

#include <chrono>
#include <string>
#include <string_view>

namespace pbr {

// Lightweight TTFS / cold-start phase timers. Grep logs for "[startup]".
// Emitted at INFO — visible when root level is INFO/DEBUG (--debug or --startup-timing).
inline logging::Logger& StartupLog() {
  static logging::Logger log = logging::getLogger("startup");
  return log;
}

inline std::chrono::steady_clock::time_point& StartupEpoch() {
  static std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
  return epoch;
}

/** Raise root log level to INFO so StartupMark/Phase INFO lines reach the platform sink. */
inline void EnableStartupTimingLogs() {
  auto root = logging::getRootLogger();
  if (root.getLevel() > logging::Level::INFO) {
    root.setLevel(logging::Level::INFO);
  }
  StartupLog().setLevel(logging::Level::INFO);
}

inline void StartupMark(std::string_view name) {
  const auto ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - StartupEpoch()).count();
  StartupLog().info << "[startup] +" << ms << " ms mark=" << name;
}

class StartupPhase {
public:
  explicit StartupPhase(std::string_view name) : name_(name), start_(std::chrono::steady_clock::now()) {}

  ~StartupPhase() {
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
    const auto total =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - StartupEpoch()).count();
    StartupLog().info << "[startup] +" << total << " ms phase=" << name_ << " duration_ms=" << ms;
  }

  StartupPhase(const StartupPhase&) = delete;
  StartupPhase& operator=(const StartupPhase&) = delete;

private:
  std::string name_;
  std::chrono::steady_clock::time_point start_;
};

} // namespace pbr
