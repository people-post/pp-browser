#include "foundation/platform/PastedFiles.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <system_error>
#include <vector>

namespace pbr {

namespace fs = std::filesystem;

std::string NewPastedRunId() {
  std::random_device device;
  std::mt19937_64 engine((static_cast<uint64_t>(device()) << 32) ^ device() ^
                         static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(engine()));
  return text;
}

std::optional<std::string> WritePastedFile(const fs::path& dir, const std::string& bytes,
                                           const std::string& extension) {
  // A name is never handed out twice in a run, even after its file is gone: a late second release of an old path
  // cannot delete a newer file that took the same name (N1).
  static std::atomic<uint64_t> counter{0};
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    return std::nullopt;
  }
  fs::path target;
  for (int attempt = 0; attempt < 10000; ++attempt) {
    const fs::path candidate = dir / ("Pasted image " + std::to_string(++counter) + "." + extension);
    if (!fs::exists(fs::symlink_status(candidate, ec))) {
      target = candidate;
      break;
    }
  }
  if (target.empty()) {
    return std::nullopt;
  }
  bool ok = false;
  {
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (out) {
      out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      out.flush();
      ok = static_cast<bool>(out);
    }
  }
  if (!ok) {
    fs::remove(target, ec); // a partial file (disk full, ...) never stays
    return std::nullopt;
  }
  return target.string();
}

bool ReleasePastedFile(const std::string& path, const fs::path& dir) {
  if (path.empty() || dir.empty()) {
    return false;
  }
  std::error_code ec;
  const fs::file_status link_status = fs::symlink_status(path, ec);
  if (ec || link_status.type() != fs::file_type::regular) {
    return false; // missing, a link (even one inside the folder), a folder, anything but a plain file
  }
  const fs::path root = fs::weakly_canonical(dir, ec);
  if (ec) {
    return false;
  }
  const fs::path file = fs::weakly_canonical(path, ec);
  if (ec) {
    return false;
  }
  // The file resolved through every link and ".." must sit under the folder.
  const fs::path relative = file.lexically_relative(root);
  if (relative.empty() || relative == "." || *relative.begin() == "..") {
    return false;
  }
  return fs::remove(file, ec) && !ec;
}

namespace {

/** The newest write time of `entry` and everything inside it (links are not followed); `fallback` when it cannot be told. */
fs::file_time_type NewestWrite(const fs::path& entry, const fs::file_time_type fallback) {
  std::error_code ec;
  fs::file_time_type newest = fs::last_write_time(entry, ec);
  if (ec) {
    return fallback;
  }
  if (fs::is_directory(fs::symlink_status(entry, ec))) {
    for (fs::recursive_directory_iterator it(entry, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code time_ec;
      const fs::file_time_type time = fs::last_write_time(it->path(), time_ec);
      if (!time_ec && time > newest) {
        newest = time;
      }
    }
    if (ec) {
      return fallback; // could not read it all: not provably stale
    }
  }
  return newest;
}

} // namespace

size_t SweepStalePastedRuns(const fs::path& root, const fs::path& own_run_dir, const std::chrono::hours stale_after,
                            const fs::file_time_type now) {
  std::error_code ec;
  if (root.empty() || fs::is_symlink(root, ec) || !fs::is_directory(root, ec)) {
    return 0;
  }
  const fs::path own = own_run_dir.lexically_normal();
  const auto limit = std::chrono::duration_cast<fs::file_time_type::duration>(stale_after);
  std::vector<fs::path> entries;
  for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    entries.push_back(it->path());
  }
  size_t removed = 0;
  for (const fs::path& entry : entries) {
    if (entry.lexically_normal() == own) {
      continue; // this run's own folder is never swept
    }
    std::error_code entry_ec;
    // A link is removed as itself (its target is never looked at); anything else only when nothing in it is recent:
    // a folder of a run that is still going has the files it wrote lately.
    const bool link = fs::is_symlink(entry, entry_ec);
    if (!link && now - NewestWrite(entry, now) <= limit) {
      continue;
    }
    fs::remove_all(entry, entry_ec);
    if (!entry_ec) {
      ++removed;
    }
  }
  return removed;
}

} // namespace pbr
