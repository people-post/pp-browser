#include "base/data/AtomicFileWrite.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <unistd.h>

namespace pbr {

namespace {

std::string MakeTempPath(const std::filesystem::path& final_path) {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  const uint64_t token = gen();
  return final_path.string() + ".tmp." + std::to_string(static_cast<unsigned long long>(getpid())) + "." +
         std::to_string(token);
}

Roe<void> FsyncPath(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return Error("Failed to open for fsync: " + path);
  }
  if (::fsync(fd) != 0) {
    const int err = errno;
    ::close(fd);
    return Error(std::string("fsync failed: ") + std::strerror(err));
  }
  ::close(fd);
  return {};
}

Roe<void> FsyncDir(const std::filesystem::path& dir) {
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    return {};
  }
  (void)::fsync(fd);
  ::close(fd);
  return {};
}

Roe<void> WriteBytes(const std::string& path, const char* data, size_t size) {
  const std::filesystem::path final_path(path);
  std::error_code ec;
  std::filesystem::create_directories(final_path.parent_path(), ec);

  const std::string tmp_path = MakeTempPath(final_path);
  {
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      return Error("Failed to open temp file for atomic write: " + tmp_path);
    }
    out.write(data, static_cast<std::streamsize>(size));
    out.flush();
    if (!out) {
      std::error_code remove_ec;
      std::filesystem::remove(tmp_path, remove_ec);
      return Error("Failed to write temp file: " + tmp_path);
    }
  }

  if (auto synced = FsyncPath(tmp_path); !synced) {
    std::error_code remove_ec;
    std::filesystem::remove(tmp_path, remove_ec);
    return synced.error();
  }

  std::filesystem::rename(tmp_path, final_path, ec);
  if (ec) {
    std::error_code remove_ec;
    std::filesystem::remove(tmp_path, remove_ec);
    return Error("Failed to rename atomic write into place: " + path + " (" + ec.message() + ")");
  }

  (void)FsyncDir(final_path.parent_path());
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
