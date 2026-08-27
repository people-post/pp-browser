#pragma once

#include "common/Error.h"

#include <string>
#include <vector>

namespace pbr {

struct ProfileEntry {
  std::string id;
  std::string label;
};

class ProfileRegistry {
public:
  static constexpr int kSchemaVersion = 1;

  ProfileRegistry();

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

  std::string data_dir_;
  std::string active_profile_id_;
  std::string session_override_;
  std::vector<ProfileEntry> profiles_;
};

} // namespace pbr
