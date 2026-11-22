#include "foundation/platform/os/OsRestart.h"

#include <spawn.h>

#include <cstring>
#include <vector>

extern char** environ;

namespace pbr::os {

bool RestartSupported() {
  return true;
}

// A new process of the same file with the same argv and environment (no LaunchServices: `open` would drop the
// environment). Not execve: an exec'd GUI process keeps the old pid's LaunchServices registration, which leaves the
// window without a Dock tile / menu bar. The caller ends this process right after.
bool RestartProcess(const RestartPlan& plan, std::string& error) {
  if (!plan.Ready()) {
    error = "no restart plan";
    return false;
  }
  std::vector<char*> args;
  args.reserve(plan.argv.size() + 1);
  for (const std::string& arg : plan.argv) {
    args.push_back(const_cast<char*>(arg.c_str()));
  }
  args.push_back(nullptr);
  pid_t pid = 0;
  const int rc = posix_spawn(&pid, plan.exe.c_str(), nullptr, nullptr, args.data(), environ);
  if (rc != 0) {
    error = std::string("posix_spawn failed: ") + std::strerror(rc);
    return false;
  }
  return true;
}

} // namespace pbr::os
