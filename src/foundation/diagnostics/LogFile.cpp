#include "foundation/diagnostics/LogFile.h"

#include "common/Logger.h"
#include "common/PbrCompat.h"

#include <filesystem>
#include <memory>
#include <stdexcept>

namespace pbr {

namespace fs = std::filesystem;

std::string LogFile::DefaultPath(const std::string& data_dir) {
  return (fs::path(data_dir) / "logs" / "pp-browser.log").string();
}

std::string LogFile::RotatedPath(const std::string& path, const std::size_t index) {
  const fs::path p(path);
  const std::string name = p.stem().string() + "." + std::to_string(index) + p.extension().string();
  return (p.parent_path() / name).string();
}

void LogFile::Rotate(const std::string& path, const std::size_t keep) {
  std::error_code ec;
  if (keep == 0) {
    fs::remove(path, ec);
    return;
  }
  fs::remove(RotatedPath(path, keep), ec);
  for (std::size_t i = keep - 1; i >= 1; --i) {
    const std::string from = RotatedPath(path, i);
    if (fs::exists(from, ec)) {
      fs::rename(from, RotatedPath(path, i + 1), ec);
    }
  }
  if (fs::exists(path, ec)) {
    fs::rename(path, RotatedPath(path, 1), ec);
  }
}

std::string LogFile::Install(const std::string& path, const std::size_t keep) {
  auto root = logging::getRootLogger();
  if (path.empty()) {
    return {};
  }
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  Rotate(path, keep);

  std::shared_ptr<logging::FileHandler> handler;
  try {
    handler = std::make_shared<logging::FileHandler>(path);
  } catch (const std::exception& ex) {
    root.warning << "Log file disabled: " << ex.what();
    return {};
  }
  // Logs carry PeerIds / account ids — keep them owner-only (no-op on Windows ACLs).
  fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
  root.addHandler(std::move(handler));
  return path;
}

} // namespace pbr
