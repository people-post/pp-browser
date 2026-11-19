#include "feature/settings/RunLogPrefs.h"

#include "foundation/data/UserPreferences.h"

namespace pbr {

std::optional<bool> RunLogPrefs::ReadCollect(const std::string& data_dir, const bool allowed) {
  if (!allowed) {
    return false;
  }
  auto machine = UserPreferences::LoadMachine(data_dir);
  if (!machine) {
    return std::nullopt;
  }
  return machine->collect_run_logs;
}

LogRetention RunLogPrefs::Retention(const std::string& data_dir, const bool allowed) {
  return LogFile::EffectiveRetention(ReadCollect(data_dir, allowed).value_or(false));
}

bool RunLogPrefs::PruneAtStartup(const std::string& data_dir, const std::string& log_path, const bool allowed) {
  const std::optional<bool> collect = ReadCollect(data_dir, allowed);
  if (!collect || log_path.empty()) {
    return false;
  }
  LogFile::PruneAll(log_path, LogFile::EffectiveRetention(*collect));
  return true;
}

Roe<void> RunLogPrefs::SetCollect(const std::string& data_dir, const std::string& log_path, const bool on) {
  auto machine = UserPreferences::LoadMachine(data_dir);
  if (!machine) {
    return machine.error();
  }
  machine->collect_run_logs = on;
  if (auto saved = UserPreferences::SaveMachine(data_dir, *machine); !saved) {
    return saved.error();
  }
  if (!on) {
    LogFile::PruneAll(log_path, LogFile::EffectiveRetention(false));
  }
  return {};
}

} // namespace pbr
