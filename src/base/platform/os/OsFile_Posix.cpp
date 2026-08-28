#if !defined(_WIN32)

#include "base/platform/os/OsFile.h"

#include "common/Error.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include "common/PbrCompat.h"

namespace pbr::os {

uint64_t GetPid() {
  return static_cast<uint64_t>(::getpid());
}

Roe<void> FsyncFile(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return Error("Failed to open for fsync: " + path.string());
  }
  if (::fsync(fd) != 0) {
    const int err = errno;
    ::close(fd);
    return Error(std::string("fsync failed: ") + std::strerror(err));
  }
  ::close(fd);
  return {};
}

void FsyncDirectory(const std::filesystem::path& dir) {
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    return;
  }
  (void)::fsync(fd);
  ::close(fd);
}

Roe<void> AtomicRename(const std::filesystem::path& tmp_path, const std::filesystem::path& final_path) {
  std::error_code ec;
  std::filesystem::rename(tmp_path, final_path, ec);
  if (ec) {
    return Error("Failed to rename atomic write into place: " + final_path.string() + " (" + ec.message() + ")");
  }
  return {};
}

} // namespace pbr::os

#endif
