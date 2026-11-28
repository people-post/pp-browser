#if !defined(_WIN32)

#include "foundation/platform/os/OsDirLock.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <unistd.h>

namespace pbr::os {

struct OsDirLock::Handle {
  int fd = -1;
};

Roe<std::unique_ptr<OsDirLock>> OsDirLock::Acquire(const std::string& lock_file) {
  const int fd = ::open(lock_file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) {
    return Error("cannot open " + lock_file + ": " + std::strerror(errno));
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int err = errno;
    ::close(fd);
    if (err == EWOULDBLOCK) {
      return Error(std::string(kHeldElsewhere) + ": " + lock_file);
    }
    return Error("cannot lock " + lock_file + ": " + std::strerror(err));
  }
  // Who holds it, for whoever finds the folder busy (informational; the lock is the flock).
  const std::string pid = std::to_string(::getpid()) + "\n";
  if (::ftruncate(fd, 0) == 0) {
    (void)!::write(fd, pid.data(), pid.size());
  }
  std::unique_ptr<OsDirLock> lock(new OsDirLock());
  lock->handle_ = std::make_unique<Handle>();
  lock->handle_->fd = fd;
  return lock;
}

OsDirLock::~OsDirLock() {
  if (handle_ && handle_->fd >= 0) {
    ::close(handle_->fd); // drops the flock
  }
}

} // namespace pbr::os

#endif
