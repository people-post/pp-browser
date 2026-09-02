#include "base/data/AtomicFileWrite.h"

#include "foundation/platform/os/OsFile.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include "common/PbrCompat.h"

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
