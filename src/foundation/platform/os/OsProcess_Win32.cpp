#if defined(_WIN32)

#include "foundation/platform/os/OsProcess.h"

namespace pbr::os {

OsProcessPipe::~OsProcessPipe() {
  Stop();
}

bool OsProcessPipe::Start(const std::string& /*command*/, const std::vector<std::string>& /*args*/) {
  Stop();
  return false;
}

void OsProcessPipe::Stop() {
  active_ = false;
}

int OsProcessPipe::Write(const void* /*data*/, size_t /*size*/) {
  return -1;
}

int OsProcessPipe::Read(void* /*buffer*/, size_t /*size*/) {
  return -1;
}

} // namespace pbr::os

#endif
