#include "platform/PlatformDefaults.h"

#include "platform/ICredentialStore.h"

#include <cstdlib>

namespace pbr {

namespace {

std::string ReadEnv(const char* name) {
  if (const char* value = std::getenv(name)) {
    return value;
  }
  return {};
}

AppConfig DesktopDefaults() {
  AppConfig config;
  config.llm.base_url = "http://localhost:11434/v1";
  config.llm.model = ReadEnv("PP_BROWSER_LLM_MODEL");
  if (config.llm.model.empty()) {
    config.llm.model = "llama3.2";
  }
  config.llm.require_api_key = false;
  config.theme = "themes/base.rcss";
  config.search.provider = "duckduckgo";
  return config;
}

} // namespace

AppConfig PlatformDefaults::For(PlatformKind kind) {
  switch (kind) {
  case PlatformKind::Desktop:
    return DesktopDefaults();
  case PlatformKind::Android:
  case PlatformKind::IOS:
    // Documented future defaults; desktop build does not ship mobile yet.
    return DesktopDefaults();
  }
  return DesktopDefaults();
}

} // namespace pbr
