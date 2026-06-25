#pragma once

#include "common/Error.h"

#include <string>

namespace pbr {

struct SafeAreaInsets {
  int top = 0;
  int bottom = 0;
  int left = 0;
  int right = 0;
};

struct WindowPrefs {
  int width = 1280;
  int height = 720;
};

struct DisplayPrefs {
  bool fullscreen = false;
};

struct MachinePreferences {
  static constexpr int kSchemaVersion = 1;

  int schema_version = kSchemaVersion;
  std::string active_profile_id = "default";
  WindowPrefs window;
  SafeAreaInsets safe_area;
  DisplayPrefs display;
};

struct ProfilePreferences {
  static constexpr int kSchemaVersion = 2;

  int schema_version = kSchemaVersion;
  std::string theme = "themes/base.rcss";
  std::string appearance = "system";
};

class UserPreferences {
public:
  static Roe<MachinePreferences> LoadMachine(const std::string& data_dir);
  static Roe<void> SaveMachine(const std::string& data_dir, const MachinePreferences& prefs);

  static Roe<ProfilePreferences> LoadProfile(const std::string& profile_data_dir);
  static Roe<void> SaveProfile(const std::string& profile_data_dir, const ProfilePreferences& prefs);

  static MachinePreferences DefaultMachine();
  static ProfilePreferences DefaultProfile();
};

} // namespace pbr
