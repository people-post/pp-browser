#include "foundation/platform/PastedFiles.h"

#include <fstream>
#include <system_error>
#include <vector>

namespace pbr {

namespace fs = std::filesystem;

std::optional<std::string> WritePastedFile(const fs::path& dir, const std::string& bytes,
                                           const std::string& extension) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    return std::nullopt;
  }
  fs::path target;
  for (int n = 1; n < 10000; ++n) {
    target = dir / ("Pasted image " + std::to_string(n) + "." + extension);
    if (!fs::exists(fs::symlink_status(target, ec))) {
      break;
    }
    target.clear();
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

size_t SweepPastedDir(const fs::path& dir) {
  std::error_code ec;
  if (dir.empty() || fs::is_symlink(dir, ec) || !fs::is_directory(dir, ec)) {
    return 0;
  }
  std::vector<fs::path> entries;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    entries.push_back(it->path());
  }
  size_t removed = 0;
  for (const fs::path& entry : entries) {
    std::error_code remove_ec;
    fs::remove_all(entry, remove_ec); // a link inside is removed itself, never followed
    if (!remove_ec) {
      ++removed;
    }
  }
  return removed;
}

} // namespace pbr
