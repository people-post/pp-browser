#include "base/data/PlatformDefaults.h"

#include "base/platform/DeploymentProfile.h"

#include <cstdlib>

namespace pbr {

namespace {

std::string ReadEnv(const char* name) {
  if (const char* value = std::getenv(name)) {
    return value;
  }
  return {};
}

AppConfig BriefDefaults() {
  AppConfig config;
  config.llm.preset = "brief";
  config.llm.base_url = BriefLlmBaseUrl();
  config.llm.model = ReadEnv("PP_BROWSER_LLM_MODEL");
  if (config.llm.model.empty()) {
    config.llm.model = "xai";
  }
  config.llm.require_api_key = true;
  config.theme = "themes/base.rcss";
  config.search.provider = "duckduckgo";
  config.promoted_mcp.url = BriefMcpUrl();
  const std::string relay_base = BriefRelayBaseUrl();
  config.relay.base_url = relay_base;
  config.directory.base_url = relay_base;
  config.registration.base_url = relay_base;
  return config;
}

} // namespace

AppConfig PlatformDefaults::For(PlatformKind /*kind*/) {
  return BriefDefaults();
}

} // namespace pbr
