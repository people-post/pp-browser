#pragma once

#include "common/Error.h"
#include "foundation/data/Config.h"

#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The services a profile chose for itself (`profiles/{id}/services.json`, profile-model PM011): the directory providers,
 * the mesh node list source and the promoted MCP. **A key absent is the backend's**: its table's address, followed as the
 * build changes it, never copied into the profile. Only what the user set is kept here, never in config.json.
 */
struct ProfileServices {
  static constexpr int kSchemaVersion = 1;

  /** The directory providers, the first primary (absent: the backend's). */
  std::optional<std::vector<std::string>> directory_providers;
  /** The mesh node list source, a directory's base URL (absent: the backend's). */
  std::optional<std::string> mesh_nodes_url;
  /** The promoted MCP the user set in Integrations (absent: the backend's seed); always `user_set`. */
  std::optional<McpConfig> promoted_mcp;

  bool operator==(const ProfileServices&) const = default;
};

/** The profile's file; a missing one is all absent; a newer one is all absent too, and never written over. */
ProfileServices LoadProfileServices(const std::string& profile_data_dir);
/** Writes the file whole (nothing set: removes it). A newer app's file is left as it is. */
Roe<void> SaveProfileServices(const std::string& profile_data_dir, const ProfileServices& services);

/**
 * `config` as this profile runs it: its own promoted MCP over the backend's seed (marked `user_set`). The directory and
 * mesh node choices reach the hub's settings instead (HubSettingsFor), never a config a save writes to config.json.
 */
void OverlayProfileServices(AppConfig& config, const ProfileServices& services);

} // namespace pbr
