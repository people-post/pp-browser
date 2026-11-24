#include "foundation/data/PlatformDefaults.h"

#include "foundation/platform/BackendTable.h"
#include "foundation/platform/DeploymentProfile.h"

#include <cstdlib>
#include <utility>

namespace pbr {

namespace {

std::string ReadEnv(const char* name) {
  if (const char* value = std::getenv(name)) {
    return value;
  }
  return {};
}

AppConfig BriefDefaults(const BackendTable& table) {
  AppConfig config;
  config.llm.preset = "brief";
  config.llm.base_url = table.llm_gateway_url;
  config.llm.model = ReadEnv("PP_BROWSER_LLM_MODEL");
  if (config.llm.model.empty()) {
    config.llm.model = "xai";
  }
  config.llm.require_api_key = true;
  config.theme = "themes/base.rcss";
  config.search.provider = "duckduckgo";
  config.promoted_mcp.url = table.promoted_mcp_url;
  config.relay.base_url = table.org_relay_url;
  config.directory.base_url = table.DirectoryUrl();
  if (table.directory_providers.size() > 1) {
    for (const std::string& url : table.directory_providers) {
      ServiceEndpointConfig provider;
      provider.base_url = url;
      config.directory.providers.push_back(std::move(provider));
    }
  }
  config.registration.base_url = table.registration_url;
  // The org relay's address carries its relay id (MR008): the table's, which a custom backend names itself — or none.
  config.relay.relay_id = table.org_relay_id;
  config.registration.relay_id = table.registration_relay_id;
  config.relay_content_sha256 = CompiledRelayContentSha256();
  return config;
}

} // namespace

AppConfig PlatformDefaults::For(PlatformKind /*kind*/) {
  return BriefDefaults(DefaultBackendTable());
}

AppConfig PlatformDefaults::For(PlatformKind /*kind*/, const std::string& origin) {
  // The build's relay id for the backend whose origin this is (MR008); another origin (a test's) names none.
  const std::string relay_id = origin == BriefOrigin() ? BriefOrgRelayIdentity().relay_id : std::string();
  return BriefDefaults(OriginBackendTable(origin, relay_id, ThisBuildKind()));
}

AppConfig PlatformDefaults::ForTable(const BackendTable& table) {
  return BriefDefaults(table);
}

} // namespace pbr
