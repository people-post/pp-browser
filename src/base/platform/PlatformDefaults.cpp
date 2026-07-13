#include "base/platform/PlatformDefaults.h"

#include <cstdlib>

namespace pbr {

namespace {

constexpr const char kBriefOrigin[] = "https://www.brief.global";

std::string ReadEnv(const char* name) {
  if (const char* value = std::getenv(name)) {
    return value;
  }
  return {};
}

AppConfig BriefDefaults() {
  AppConfig config;
  config.llm.preset = "brief";
  config.llm.base_url = std::string(kBriefOrigin) + "/api/llm/v1";
  config.llm.model = ReadEnv("PP_BROWSER_LLM_MODEL");
  if (config.llm.model.empty()) {
    config.llm.model = "grok-4-1-fast-reasoning";
  }
  config.llm.require_api_key = true;
  config.theme = "themes/base.rcss";
  config.search.provider = "duckduckgo";
  config.promoted_mcp.url = std::string(kBriefOrigin) + "/mcp";
  const std::string relay_base = std::string(kBriefOrigin) + "/api/relay";
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
