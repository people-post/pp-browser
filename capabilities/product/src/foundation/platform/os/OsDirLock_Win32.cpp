#if defined(_WIN32)

#include "foundation/platform/os/OsDirLock.h"

#include <windows.h>

#include <string>

namespace pbr::os {

struct OsDirLock::Handle {
  HANDLE file = INVALID_HANDLE_VALUE;
};

namespace {

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
  return out;
}

} // namespace

Roe<std::unique_ptr<OsDirLock>> OsDirLock::Acquire(const std::string& lock_file) {
  const HANDLE file = CreateFileW(Widen(lock_file).c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return Error("cannot open " + lock_file + ": error " + std::to_string(GetLastError()));
  }
  OVERLAPPED at{};
  if (!LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at)) {
    const DWORD err = GetLastError();
    CloseHandle(file);
    if (err == ERROR_LOCK_VIOLATION || err == ERROR_IO_PENDING) {
      return Error(std::string(kHeldElsewhere) + ": " + lock_file);
    }
    return Error("cannot lock " + lock_file + ": error " + std::to_string(err));
  }
  std::unique_ptr<OsDirLock> lock(new OsDirLock());
  lock->handle_ = std::make_unique<Handle>();
  lock->handle_->file = file;
  return lock;
}

OsDirLock::~OsDirLock() {
  if (handle_ && handle_->file != INVALID_HANDLE_VALUE) {
    CloseHandle(handle_->file); // drops the lock
  }
}

} // namespace pbr::os

#endif
