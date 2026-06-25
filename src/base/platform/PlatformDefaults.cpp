#include "base/platform/PlatformDefaults.h"

#include <cstdlib>

namespace pbr {

namespace {

std::string ReadEnv(const char* name) {
  if (const char* value = std::getenv(name)) {
    return value;
  }
  return {};
}

AppConfig CloudDefaults() {
  AppConfig config;
  config.llm.base_url = "https://api.openai.com/v1";
  config.llm.model = ReadEnv("PP_BROWSER_LLM_MODEL");
  if (config.llm.model.empty()) {
    config.llm.model = "gpt-4o-mini";
  }
  config.llm.require_api_key = true;
  config.theme = "themes/base.rcss";
  config.search.provider = "duckduckgo";
  config.promoted_mcp.url = "https://www.brief.global/mcp";
  return config;
}

} // namespace

AppConfig PlatformDefaults::For(PlatformKind /*kind*/) {
  return CloudDefaults();
}

} // namespace pbr
