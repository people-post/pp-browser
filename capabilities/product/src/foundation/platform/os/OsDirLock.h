#pragma once

#include "common/Error.h"

#include <memory>
#include <string>
#include "common/PbrCompat.h"

namespace pbr::os {

/**
 * An exclusive lock on a file, held until destroyed or the process ends (the OS drops it then, so a crash never leaves
 * it behind). pp-node holds one on its data root so a second process on the same folder exits instead of running the
 * same identity twice (projects/node-relay NR001: one data folder, one build at a time). POSIX flock, Windows LockFileEx;
 * both lock the open file, so a second Acquire in the same process fails too.
 */
class OsDirLock {
public:
  /** Creates `lock_file` when missing and locks it, or fails at once when another holder has it. */
  static Roe<std::unique_ptr<OsDirLock>> Acquire(const std::string& lock_file);
  ~OsDirLock();
  OsDirLock(const OsDirLock&) = delete;
  OsDirLock& operator=(const OsDirLock&) = delete;

  /** Acquire's error when another holder has the lock (not an I/O failure). */
  static constexpr const char* kHeldElsewhere = "held by another process";

private:
  OsDirLock() = default;
  struct Handle;
  std::unique_ptr<Handle> handle_;
};

} // namespace pbr::os
