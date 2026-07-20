#if defined(_WIN32)

#include "base/platform/os/OsFile.h"

#include "common/Error.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h>
#include <windows.h>

namespace pbr::os {

uint64_t GetPid() {
  return static_cast<uint64_t>(_getpid());
}

Roe<void> FsyncFile(const std::filesystem::path& path) {
  const HANDLE handle =
      CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Error("Failed to open for fsync: " + path.string());
  }
  if (!FlushFileBuffers(handle)) {
    CloseHandle(handle);
    return Error("fsync failed: " + path.string());
  }
  CloseHandle(handle);
  return {};
}

void FsyncDirectory(const std::filesystem::path& dir) {
  const HANDLE handle =
      CreateFileW(dir.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return;
  }
  (void)FlushFileBuffers(handle);
  CloseHandle(handle);
}

Roe<void> AtomicRename(const std::filesystem::path& tmp_path, const std::filesystem::path& final_path) {
  if (!MoveFileExW(tmp_path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return Error("Failed to rename atomic write into place: " + final_path.string());
  }
  return {};
}

} // namespace pbr::os

#endif
