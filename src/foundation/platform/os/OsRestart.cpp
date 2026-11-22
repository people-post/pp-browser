#include "foundation/platform/os/OsRestart.h"

#include <mutex>

namespace pbr::os {

namespace {

std::mutex& StartMutex() {
  static std::mutex mutex;
  return mutex;
}

std::vector<std::string>& StartStorage() {
  static std::vector<std::string> args;
  return args;
}

bool& StartCaptured() {
  static bool captured = false;
  return captured;
}

} // namespace

void CaptureStartArguments(const int argc, char** argv) {
  std::lock_guard<std::mutex> lock(StartMutex());
  if (StartCaptured() || argc <= 0 || argv == nullptr) {
    return;
  }
  StartCaptured() = true;
  for (int i = 0; i < argc; ++i) {
    StartStorage().emplace_back(argv[i] != nullptr ? argv[i] : "");
  }
}

const std::vector<std::string>& StartArguments() {
  return StartStorage();
}

RestartPlan PlanRestart(const std::string& exe_path, const std::vector<std::string>& start_args) {
  RestartPlan plan;
  if (exe_path.empty() || start_args.empty()) {
    return plan;
  }
  plan.exe = exe_path;
  plan.argv = start_args; // byte for byte, argv[0] included
  return plan;
}

} // namespace pbr::os
