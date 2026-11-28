#include "foundation/platform/desktop/PathProviderImpl.h"

#include <cstdlib>
#include <filesystem>

namespace pbr::desktop {

std::string ReadEnv(const char* name) {
  if (const char* value = std::getenv(name)) {
    return value;
  }
  return {};
}

std::string ExpandHome(std::string path) {
  if (!path.empty() && path.front() == '~') {
    const char* home = std::getenv("HOME");
    if (!home) {
      home = std::getenv("USERPROFILE");
    }
    if (home) {
      return std::string(home) + path.substr(1);
    }
  }
  return path;
}

std::string HomeDir() {
  if (const char* home = std::getenv("HOME")) {
    return home;
  }
  if (const char* profile = std::getenv("USERPROFILE")) {
    return profile;
  }
  return {};
}

} // namespace pbr::desktop
