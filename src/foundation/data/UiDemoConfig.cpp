#include "foundation/data/UiDemoConfig.h"

namespace pbr {

void ApplyUiDemoConfig(AppConfig& config) {
  config.backend = BackendConfig{};  // env = prod, no saved URLs
  config.relay = ServiceEndpointConfig{};
  config.directory = DirectoryConfig{};
  config.registration = ServiceEndpointConfig{};
  config.mesh.mesh_enabled = false;
  config.llm.base_url.clear();
  config.promoted_mcp.url.clear();
  for (McpConfig& server : config.mcp_servers) {
    server.url.clear();
  }
}

void ApplyUiDemoProfilePrefs(ProfilePreferences& prefs) {
  prefs.onboarding_completed = true;
}

} // namespace pbr
