#include "foundation/diagnostics/LogFile.h"

#include "common/Logger.h"
#include "common/Diag.h"
#include "common/Metrics.h"
#include "common/PbrCompat.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

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

std::vector<std::string> LogFile::ExpiredFiles(std::vector<LogFileInfo> files, const fs::file_time_type now,
                                               const LogRetention& retention) {
  std::stable_sort(files.begin(), files.end(),
                   [](const LogFileInfo& a, const LogFileInfo& b) { return a.mtime < b.mtime; });
  std::vector<std::string> expired;
  std::uintmax_t total = 0;
  for (const LogFileInfo& f : files) {
    total += f.size;
  }
  // The previous launch's log (the newest rotated file) is what a crash report needs: it is never deleted, by age or
  // by size, even when it alone is over the cap. It still counts toward the total.
  std::size_t newest_rotated = files.size();
  for (std::size_t i = 0; i < files.size(); ++i) {
    if (!files[i].current) {
      newest_rotated = i;
    }
  }
  for (std::size_t i = 0; i < files.size(); ++i) { // oldest first
    const LogFileInfo& f = files[i];
    if (f.current || i == newest_rotated) {
      continue;
    }
    if (now - f.mtime > retention.max_age || total > retention.max_bytes) {
      expired.push_back(f.name);
      total -= f.size;
    }
  }
  return expired;
}

namespace {

/** N of a rotated file named `<prefix><N><ext>` (`prefix` = `<stem>.`), or nothing for any other name. */
std::optional<std::size_t> RotatedIndex(const std::string& name, const std::string& prefix, const std::string& ext) {
  if (name.size() <= prefix.size() + ext.size() || name.compare(0, prefix.size(), prefix) != 0 ||
      name.compare(name.size() - ext.size(), ext.size(), ext) != 0) {
    return std::nullopt;
  }
  const std::string digits = name.substr(prefix.size(), name.size() - prefix.size() - ext.size());
  if (digits.size() < 9 && digits.find_first_not_of("0123456789") == std::string::npos) {
    return static_cast<std::size_t>(std::stoul(digits));
  }
  return std::nullopt;
}

/** The indexes of `path`'s rotated files present in its directory. */
std::vector<std::size_t> RotatedIndexes(const std::string& path) {
  const fs::path p(path);
  const std::string prefix = p.stem().string() + ".";
  const std::string ext = p.extension().string();
  const fs::path dir = p.parent_path().empty() ? fs::path(".") : p.parent_path();
  std::error_code ec;
  std::vector<std::size_t> indexes;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (const auto index = RotatedIndex(it->path().filename().string(), prefix, ext)) {
      indexes.push_back(*index);
    }
  }
  return indexes;
}

} // namespace

void LogFile::Rotate(const std::string& path) {
  std::error_code ec;
  std::vector<std::size_t> indexes = RotatedIndexes(path);
  std::sort(indexes.rbegin(), indexes.rend()); // highest first: no rename lands on a file not yet moved
  for (const std::size_t i : indexes) {
    fs::rename(RotatedPath(path, i), RotatedPath(path, i + 1), ec);
  }
  if (fs::exists(path, ec)) {
    fs::rename(path, RotatedPath(path, 1), ec);
  }
}

std::vector<LogFileInfo> LogFile::FamilyFiles(const std::string& path) {
  std::vector<std::pair<std::string, bool>> names;
  std::error_code ec;
  if (fs::exists(path, ec)) {
    names.emplace_back(path, true);
  }
  for (const std::size_t i : RotatedIndexes(path)) {
    names.emplace_back(RotatedPath(path, i), false);
  }
  std::vector<LogFileInfo> files;
  for (const auto& [name, current] : names) {
    ec.clear();
    LogFileInfo info;
    info.name = name;
    info.current = current;
    info.mtime = fs::last_write_time(name, ec);
    if (ec) {
      continue;
    }
    info.size = fs::file_size(name, ec);
    if (!ec) {
      files.push_back(std::move(info));
    }
  }
  return files;
}

void LogFile::Prune(const std::string& path, const LogRetention& retention) {
  std::error_code ec;
  for (const std::string& name : ExpiredFiles(FamilyFiles(path), fs::file_time_type::clock::now(), retention)) {
    fs::remove(name, ec);
  }
}

void LogFile::PruneAll(const std::string& log_path, const LogRetention& retention) {
  if (log_path.empty()) {
    return;
  }
  Prune(log_path, retention);
  Prune(MetricsPath(log_path), retention);
  Prune(DiagPath(log_path), retention);
}

LogRetention LogFile::EffectiveRetention(const bool collect) {
  LogRetention retention;
  if (!collect) {
    retention.max_age = std::chrono::hours(0);
  }
  return retention;
}

namespace {

std::mutex& InstalledMutex() {
  static std::mutex mutex;
  return mutex;
}

std::string& InstalledPathSlot() {
  static std::string path;
  return path;
}

// Creates `path` (empty, append mode) and makes it owner-only before anyone writes to it.
void CreateOwnerOnly(const std::string& path) {
  {
    std::ofstream touch(path, std::ios::app);
  }
  std::error_code ec;
  fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
}

} // namespace

std::string LogFile::Install(const std::string& path) {
  auto root = logging::getRootLogger();
  if (path.empty()) {
    return {};
  }
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  Rotate(path);
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
  {
    std::lock_guard<std::mutex> lock(InstalledMutex());
    InstalledPathSlot() = path;
  }
  return path;
}

std::string LogFile::InstalledPath() {
  std::lock_guard<std::mutex> lock(InstalledMutex());
  return InstalledPathSlot();
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
std::string InstallChannelFile(const std::string& path, logging::Logger& channel) {
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  LogFile::Rotate(path);
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

std::string LogFile::InstallMetrics(const std::string& log_path) {
  return log_path.empty() ? std::string() : InstallChannelFile(MetricsPath(log_path), MetricsLog());
}

std::string LogFile::InstallDiag(const std::string& log_path) {
  return log_path.empty() ? std::string() : InstallChannelFile(DiagPath(log_path), DiagLog());
}

} // namespace pbr
