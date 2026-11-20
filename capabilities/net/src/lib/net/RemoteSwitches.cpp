#include "lib/net/RemoteSwitches.h"

namespace pbr {

std::mutex& RemoteSwitches::Mutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, bool>& RemoteSwitches::Switches() {
  static std::map<std::string, bool> switches;
  return switches;
}

bool& RemoteSwitches::LoadedFlag() {
  static bool loaded = false;
  return loaded;
}

void RemoteSwitches::Load(const std::map<std::string, bool>& switches) {
  std::lock_guard lock(Mutex());
  if (LoadedFlag()) {
    return; // read once a run
  }
  Switches() = switches;
  LoadedFlag() = true;
}

bool RemoteSwitches::IsOn(const std::string& name, const bool built_in_default) {
  std::lock_guard lock(Mutex());
  const auto found = Switches().find(name);
  return found == Switches().end() ? built_in_default : found->second;
}

bool RemoteSwitches::Loaded() {
  std::lock_guard lock(Mutex());
  return LoadedFlag();
}

void RemoteSwitches::ResetForTest() {
  std::lock_guard lock(Mutex());
  Switches().clear();
  LoadedFlag() = false;
}

} // namespace pbr
