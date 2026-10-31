#include "foundation/diagnostics/LogFile.h"

#include "common/Logger.h"
#include "common/Diag.h"
#include "common/Metrics.h"
#include "common/PbrCompat.h"

#include <filesystem>
#include <fstream>
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

namespace {

// Creates `path` (empty, append mode) and makes it owner-only before anyone writes to it.
void CreateOwnerOnly(const std::string& path) {
  {
    std::ofstream touch(path, std::ios::app);
  }
  std::error_code ec;
  fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
}

} // namespace

std::string LogFile::Install(const std::string& path, const std::size_t keep) {
  auto root = logging::getRootLogger();
  if (path.empty()) {
    return {};
  }
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  Rotate(path, keep);
  // Logs carry PeerIds / account ids — keep them owner-only (no-op on Windows ACLs). The file is created
  // and locked down before the handler opens it, so it never exists with umask permissions (PB-062).
  CreateOwnerOnly(path);

  std::shared_ptr<logging::FileHandler> handler;
  try {
    handler = std::make_shared<logging::FileHandler>(path);
  } catch (const std::exception& ex) {
    root.warning << "Log file disabled: " << ex.what();
    return {};
  }
  root.addHandler(std::move(handler));
  return path;
}

std::string LogFile::MetricsPath(const std::string& log_path) {
  return (fs::path(log_path).parent_path() / "metrics.log").string();
}

std::string LogFile::DiagPath(const std::string& log_path) {
  return (fs::path(log_path).parent_path() / "diag.log").string();
}

namespace {

/**
 * Rotate `path` and attach it to `channel` as the channel's own handler: its INFO lines reach this file
 * whatever the root level (the root filters them out of the main log unless the app runs with --debug).
 */
std::string InstallChannelFile(const std::string& path, logging::Logger& channel, const std::size_t keep) {
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  LogFile::Rotate(path, keep);
  CreateOwnerOnly(path);

  std::shared_ptr<logging::FileHandler> handler;
  try {
    handler = std::make_shared<logging::FileHandler>(path);
  } catch (const std::exception& ex) {
    logging::getRootLogger().warning << "Channel file disabled: " << fs::path(path).filename().string() << ": "
                                     << ex.what();
    return {};
  }
  channel.addHandler(std::move(handler));
  return path;
}

} // namespace

std::string LogFile::InstallMetrics(const std::string& log_path, const std::size_t keep) {
  return log_path.empty() ? std::string() : InstallChannelFile(MetricsPath(log_path), MetricsLog(), keep);
}

std::string LogFile::InstallDiag(const std::string& log_path, const std::size_t keep) {
  return log_path.empty() ? std::string() : InstallChannelFile(DiagPath(log_path), DiagLog(), keep);
}

} // namespace pbr
