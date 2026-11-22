#include "foundation/platform/os/OsRestart.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>

#include <vector>

extern char** environ;

namespace pbr::os {

bool RestartSupported() {
  return true;
}

// The process replaces itself: pid, environment and open inheritable descriptors stay as they are.
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
  execve(plan.exe.c_str(), args.data(), environ);
  error = std::string("execve failed: ") + std::strerror(errno);
  return false;
}

} // namespace pbr::os
