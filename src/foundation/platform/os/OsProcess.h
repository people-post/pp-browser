#pragma once

#include "common/Error.h"

#include <cstddef>
#include <string>
#include <vector>

namespace pbr::os {

class OsProcessPipe {
public:
  OsProcessPipe() = default;
  ~OsProcessPipe();

  OsProcessPipe(const OsProcessPipe&) = delete;
  OsProcessPipe& operator=(const OsProcessPipe&) = delete;

  bool Start(const std::string& command, const std::vector<std::string>& args);
  void Stop();
  bool IsActive() const { return active_; }

  int Write(const void* data, size_t size);
  int Read(void* buffer, size_t size);

private:
  bool active_ = false;
#if defined(_WIN32)
  // Subprocess stdio is not implemented on Windows; MCP uses HTTP or mock.
#else
  int child_pid_ = -1;
  int stdin_write_fd_ = -1;
  int stdout_read_fd_ = -1;
#endif
};

} // namespace pbr::os
