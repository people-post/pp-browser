#pragma once

#include "app/Config.h"
#include "app/ProfileRegistry.h"
#include "app/UserPreferences.h"
#include "common/Error.h"

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
};

class Bootstrap {
public:
  static Roe<BootstrapResult> Run(const BootstrapOptions& options);
};

} // namespace pbr
