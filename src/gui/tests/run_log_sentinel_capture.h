#pragma once

// "Send run logs" sentinels (DEV/PLAN-send-run-logs.md step 1): what the gui tests use to prove typed words never reach
// the log. The cases: run_log_sentinel_test.cpp, ask_ai_panel_test.cpp, shell_host_test.cpp (RunLogSentinel*).

#include "common/Logger.h"

#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pbr::run_log_sentinel {

inline constexpr const char* kSentinel = "zq-sentinel-9147-雪";

/** Every line any logger writes, at the lowest level (one handler for the process: handlers cannot be removed). */
class Capture : public logging::Handler {
public:
  /** Every logger at DEBUG, the capture emptied and checked live (a DEBUG line it must see). */
  static Capture& Start() {
    static const std::shared_ptr<Capture> capture = [] {
      auto made = std::make_shared<Capture>();
      logging::getRootLogger().addHandler(made);
      return made;
    }();
    logging::getRootLogger().setLevel(logging::Level::DEBUG);
    logging::setEmitFloor(logging::Level::DEBUG);
    {
      std::lock_guard<std::mutex> lock(capture->mutex_);
      capture->lines_.clear();
    }
    logging::getRootLogger().debug << "run-log-sentinel-control";
    EXPECT_EQ(capture->LinesWith("run-log-sentinel-control").size(), 1u) << "the capture sees DEBUG lines";
    return *capture;
  }
  void emit(logging::Level, const std::string&, const std::string& message) override {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.push_back(message);
  }
  std::vector<std::string> LinesWith(const std::string& needle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> found;
    for (const std::string& line : lines_) {
      if (line.find(needle) != std::string::npos) {
        found.push_back(line);
      }
    }
    return found;
  }
  /** Fails the test for each line that holds the sentinel. */
  void ExpectNoSentinel() const {
    for (const std::string& line : LinesWith(kSentinel)) {
      ADD_FAILURE() << "user text reached the log: " << line;
    }
  }

private:
  mutable std::mutex mutex_;
  std::vector<std::string> lines_;
};

} // namespace pbr::run_log_sentinel
