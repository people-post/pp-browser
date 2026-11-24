#pragma once

#include "common/Error.h"
#include "foundation/data/ProfileManifest.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

struct ProfileEntry {
  std::string id;
  std::string label;
};

/**
 * The profiles under `{data_dir}/profiles/` and which one each backend uses (profile-model PM001, PM003, PM008,
 * PM009, PM012). A profile's folder name is an opaque id, fixed for its life (every sealed file's AAD and the keychain
 * items name it); its manifest records its network and is the only record of it; `backends/{backend_id}/` in it says
 * which backend it is the home profile of. `profiles.json` holds the labels and one pointer per backend (`active`),
 * never a network. Nothing is ever adopted or renamed: a folder whose manifest is missing, unreadable or newer is left
 * alone, and a backend without a profile gets a new one.
 */
class ProfileRegistry {
public:
  static constexpr int kSchemaVersion = 2;

  ProfileRegistry();

  /**
   * A profile id names a directory under `profiles/`, a backend id a directory under its `backends/`: 1–64 letters,
   * digits, `.`, `_` or `-`, not starting with `.` — so nothing read from a file can reach outside them.
   */
  static bool IsValidProfileId(std::string_view id);

  /**
   * Reads `profiles.json` and every profile's manifest. Creates nothing; a `profiles.json` of an older schema is read
   * as empty (its old pointer kept only for pp-node's conversion), a newer one refused.
   */
  static Roe<ProfileRegistry> Load(const std::string& data_dir);
  static Roe<void> Save(const std::string& data_dir, const ProfileRegistry& registry);

  /** Profiles whose manifest is the old format (schema 1, no network): the app removes them, then ForgetProfile. */
  std::vector<std::string> OldFormatProfiles() const;
  /** Folders under `profiles/` that are left alone: no manifest, an unreadable one, or a newer one. */
  std::vector<std::string> SkippedProfiles() const;
  /** Forgets a profile whose folder the caller removed: its entry and every pointer to it. Saves. */
  Roe<void> ForgetProfile(const std::string& id);

  /**
   * pp-node only (PM007): every old-format profile written again on network `node`, in place. The old `profiles.json`'s
   * active one (else `default`) becomes `node`'s home profile; the others keep their old id as a label, so
   * `--profile <old id>` still finds them. A deployed node keeps its identity. Saves when anything changed.
   */
  Roe<void> ConvertOldFormatProfilesToNode();

  /**
   * Makes the active profile `backend_id`'s home profile on `network`: the one `active` points at, else an unlabelled
   * one on the network holding `backends/{backend_id}/` (a labelled one is `--profile`'s), else a new one. A pointer at a profile that cannot be read (unreadable or
   * newer manifest) is an error and is not moved, so a downgrade cannot point a backend at an empty profile. Saves when
   * the pointer or the profiles change.
   */
  Roe<void> SelectProfileForBackend(const std::string& backend_id, std::string_view network);

  /**
   * `--profile <label>` (dev isolation): the running backend's profile with that label, else a new one with it. Never a
   * folder name; the pointer is left alone. Saves when a profile is made.
   */
  Roe<void> SelectProfileByLabel(const std::string& label, const std::string& backend_id, std::string_view network);

  /**
   * Reset my data: a new profile for the active profile's backend and network takes the active one's place (its label
   * and every pointer at it); the old one is forgotten. The caller removes the old folder first. Saves.
   */
  Roe<void> ReplaceActiveProfile();

  std::string ActiveProfileId() const { return active_profile_id_; }
  std::string ActiveProfileDataDir() const;
  std::string ActiveBackendId() const { return active_backend_id_; }
  std::string ActiveNetwork() const { return active_network_; }
  std::string ProfileDataDir(const std::string& id) const;
  const std::vector<ProfileEntry>& Profiles() const { return profiles_; }
  const std::map<std::string, std::string>& BackendPointers() const { return active_; }
  ProfileManifestRead ManifestOf(const std::string& id) const;

private:
  explicit ProfileRegistry(std::string data_dir);
  bool HasBackend(const std::string& id, const std::string& backend_id) const;
  bool IsHomeOf(const std::string& id, const std::string& backend_id, std::string_view network) const;
  ProfileEntry* EntryOf(const std::string& id);
  /** A new profile on `network` with `backends/{backend_id}/`, made in a hidden folder and renamed into place. */
  Roe<std::string> CreateProfile(std::string_view network, const std::string& backend_id, const std::string& label);
  void Activate(const std::string& id, const std::string& backend_id, std::string_view network);
  void Rescan();

  std::string data_dir_;
  std::vector<ProfileEntry> profiles_;
  std::map<std::string, std::string> active_; // backend id → profile id
  std::map<std::string, ProfileManifestRead> manifests_;
  std::string legacy_active_id_; // an old profiles.json's active_profile_id (pp-node's conversion)
  std::string active_profile_id_;
  std::string active_backend_id_;
  std::string active_network_;
};

} // namespace pbr
