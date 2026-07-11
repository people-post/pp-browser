#include "base/data/AtomicFileWrite.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pbr {

namespace {

std::filesystem::path MakeTempPath(const std::filesystem::path& final_path) {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  const uint64_t token = gen();
#if defined(_WIN32)
  const auto pid = static_cast<unsigned long long>(_getpid());
#else
  const auto pid = static_cast<unsigned long long>(::getpid());
#endif
  std::filesystem::path tmp = final_path;
  tmp += ".tmp.";
  tmp += std::to_string(pid);
  tmp += ".";
  tmp += std::to_string(token);
  return tmp;
}

Roe<void> FsyncPath(const std::filesystem::path& path) {
#if defined(_WIN32)
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
#else
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
#endif
}

void FsyncDir(const std::filesystem::path& dir) {
#if defined(_WIN32)
  const HANDLE handle =
      CreateFileW(dir.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return;
  }
  (void)FlushFileBuffers(handle);
  CloseHandle(handle);
#else
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    return;
  }
  (void)::fsync(fd);
  ::close(fd);
#endif
}

Roe<void> RenameReplace(const std::filesystem::path& tmp_path, const std::filesystem::path& final_path) {
#if defined(_WIN32)
  // std::filesystem::rename does not overwrite an existing destination on Windows.
  if (!MoveFileExW(tmp_path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return Error("Failed to rename atomic write into place: " + final_path.string());
  }
  return {};
#else
  std::error_code ec;
  std::filesystem::rename(tmp_path, final_path, ec);
  if (ec) {
    return Error("Failed to rename atomic write into place: " + final_path.string() + " (" + ec.message() + ")");
  }
  return {};
#endif
}

Roe<void> WriteBytes(const std::string& path, const char* data, size_t size) {
  const std::filesystem::path final_path(path);
  std::error_code ec;
  std::filesystem::create_directories(final_path.parent_path(), ec);

  const std::filesystem::path tmp_path = MakeTempPath(final_path);
  {
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      return Error("Failed to open temp file for atomic write: " + tmp_path.string());
    }
    out.write(data, static_cast<std::streamsize>(size));
    out.flush();
    if (!out) {
      std::error_code remove_ec;
      std::filesystem::remove(tmp_path, remove_ec);
      return Error("Failed to write temp file: " + tmp_path.string());
    }
  }

  if (auto synced = FsyncPath(tmp_path); !synced) {
    std::error_code remove_ec;
    std::filesystem::remove(tmp_path, remove_ec);
    return synced.error();
  }

  if (auto renamed = RenameReplace(tmp_path, final_path); !renamed) {
    std::error_code remove_ec;
    std::filesystem::remove(tmp_path, remove_ec);
    return renamed.error();
  }

  FsyncDir(final_path.parent_path());
  return {};
}

} // namespace

Roe<void> AtomicFileWrite::Write(const std::string& path, std::string_view data) {
  return WriteBytes(path, data.data(), data.size());
}

Roe<void> AtomicFileWrite::Write(const std::string& path, const std::vector<uint8_t>& data) {
  return WriteBytes(path, reinterpret_cast<const char*>(data.data()), data.size());
}

} // namespace pbr
