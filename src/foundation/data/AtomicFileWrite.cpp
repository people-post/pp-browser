#include "foundation/data/AtomicFileWrite.h"

#include "foundation/platform/os/OsFile.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include "common/PbrCompat.h"

#if !defined(_WIN32)
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pbr {

namespace {

std::filesystem::path MakeTempPath(const std::filesystem::path& final_path) {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  const uint64_t token = gen();
  const auto pid = os::GetPid();
  std::filesystem::path tmp = final_path;
  tmp += ".tmp.";
  tmp += std::to_string(pid);
  tmp += ".";
  tmp += std::to_string(token);
  return tmp;
}

#if !defined(_WIN32)
// Profile data (config/preferences can hold secrets, e.g. LLM api_key): create the temp file
// with owner-only permissions from the start (O_CREAT mode 0600) rather than widening the
// window where it exists with default (umask-derived) permissions before a later chmod.
Roe<void> WriteBytesPosix(const std::filesystem::path& tmp_path, const char* data, size_t size) {
  const int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    return Error(std::string("Failed to open temp file for atomic write: ") + std::strerror(errno));
  }
  size_t written = 0;
  while (written < size) {
    const ssize_t n = ::write(fd, data + written, size - written);
    if (n < 0) {
      const int err = errno;
      ::close(fd);
      return Error(std::string("Failed to write temp file: ") + std::strerror(err));
    }
    written += static_cast<size_t>(n);
  }
  if (::close(fd) != 0) {
    return Error(std::string("Failed to close temp file: ") + std::strerror(errno));
  }
  return {};
}
#endif

Roe<void> WriteBytes(const std::string& path, const char* data, size_t size) {
  const std::filesystem::path final_path(path);
  std::error_code ec;
  std::filesystem::create_directories(final_path.parent_path(), ec);

  const std::filesystem::path tmp_path = MakeTempPath(final_path);
#if !defined(_WIN32)
  if (auto written = WriteBytesPosix(tmp_path, data, size); !written) {
    std::error_code remove_ec;
    std::filesystem::remove(tmp_path, remove_ec);
    return written.error();
  }
#else
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
  // See header comment on SetOwnerOnlyPermissions: Windows ACLs are not modeled here.
  os::SetOwnerOnlyPermissions(tmp_path);
#endif

  if (auto synced = os::FsyncFile(tmp_path); !synced) {
    std::error_code remove_ec;
    std::filesystem::remove(tmp_path, remove_ec);
    return synced.error();
  }

  if (auto renamed = os::AtomicRename(tmp_path, final_path); !renamed) {
    std::error_code remove_ec;
    std::filesystem::remove(tmp_path, remove_ec);
    return renamed.error();
  }

  os::FsyncDirectory(final_path.parent_path());
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
