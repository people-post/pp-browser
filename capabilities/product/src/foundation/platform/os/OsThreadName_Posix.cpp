#if !defined(_WIN32)

#include "foundation/platform/os/OsThreadName.h"

#include <pthread.h>

namespace pbr::os {

void SetCurrentThreadName(const std::string& name) {
  // Linux / Android cap names at 15 characters + NUL.
  const std::string clipped = name.substr(0, 15);
#if defined(__APPLE__)
  pthread_setname_np(clipped.c_str());
#else
  pthread_setname_np(pthread_self(), clipped.c_str());
#endif
}

} // namespace pbr::os

#endif
