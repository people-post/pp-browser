#pragma once

#include "common/Error.h"

#include <string>
#include <string_view>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

struct ProfileEntry {
  std::string id;
  std::string label;
};

class ProfileRegistry {
public:
  static constexpr int kSchemaVersion = 1;

  ProfileRegistry();

  /**
   * A profile id names a directory under `profiles/`: 1–64 letters, digits, `.`, `_` or `-`, not
   * starting with `.` — so `--profile`, PP_NODE_PROFILE or profiles.json cannot reach outside it.
   */
  static bool IsValidProfileId(std::string_view id);

  static Roe<ProfileRegistry> Load(const std::string& data_dir);
  static Roe<void> Save(const std::string& data_dir, const ProfileRegistry& registry);

  Roe<void> EnsureDefaultProfile();
  Roe<void> EnsureActiveProfile();
  std::string ActiveProfileId() const { return active_profile_id_; }
  std::string ActiveProfileDataDir() const;
  void SetActiveProfileId(std::string id) { active_profile_id_ = std::move(id); }
  void SetSessionProfileOverride(std::string id) { session_override_ = std::move(id); }
  const std::vector<ProfileEntry>& Profiles() const { return profiles_; }

  std::string DataRoot() const { return data_dir_; }

private:
  ProfileRegistry(std::string data_dir, std::string active_profile_id, std::vector<ProfileEntry> profiles);
  /** The active id (session override first) is a valid id: before anything is created for it. */
  Roe<void> CheckActiveProfileId() const;

  std::string data_dir_;
  std::string active_profile_id_;
  std::string session_override_;
  std::vector<ProfileEntry> profiles_;
};

} // namespace pbr
