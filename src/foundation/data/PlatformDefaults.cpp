#include "foundation/data/PlatformDefaults.h"

#include "foundation/platform/DeploymentProfile.h"

#include <cstdlib>

namespace pbr {

namespace {

std::string ReadEnv(const char* name) {
  if (const char* value = std::getenv(name)) {
    return value;
  }
  return {};
}

AppConfig BriefDefaults(const std::string& origin) {
  AppConfig config;
  config.llm.preset = "brief";
  config.llm.base_url = BriefLlmBaseUrlFor(origin);
  config.llm.model = ReadEnv("PP_BROWSER_LLM_MODEL");
  if (config.llm.model.empty()) {
    config.llm.model = "xai";
  }
  config.llm.require_api_key = true;
  config.theme = "themes/base.rcss";
  config.search.provider = "duckduckgo";
  config.promoted_mcp.url = BriefMcpUrlFor(origin);
  const std::string relay_base = BriefRelayBaseUrlFor(origin);
  config.relay.base_url = relay_base;
  config.directory.base_url = relay_base;
  config.registration.base_url = relay_base;
  config.relay_content_sha256 = CompiledRelayContentSha256();
  return config;
}

} // namespace

AppConfig PlatformDefaults::For(const PlatformKind kind) {
  return For(kind, BriefOrigin());
}

AppConfig PlatformDefaults::For(PlatformKind /*kind*/, const std::string& origin) {
  return BriefDefaults(origin);
}

} // namespace pbr
