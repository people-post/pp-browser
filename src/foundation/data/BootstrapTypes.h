#pragma once

#include "foundation/data/Config.h"
#include "foundation/data/ProfileRegistry.h"
#include "foundation/data/UserPreferences.h"

#include <string>

namespace pbr {

struct BootstrapResult {
  AppConfig config;
  MachinePreferences machine_prefs;
  ProfilePreferences profile_prefs;
  ProfileRegistry profile_registry;
  std::string data_dir;
  std::string profile_data_dir;
  std::string config_path;
};

struct BootstrapOptions {
  int argc = 0;
  char** argv = nullptr;
  std::string profile_override;
  /** Profile unlock PIN; falls back to PP_BROWSER_PIN. */
  std::string pin;
};

} // namespace pbr
