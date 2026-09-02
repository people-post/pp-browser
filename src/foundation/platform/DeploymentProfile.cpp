#include "foundation/platform/DeploymentProfile.h"

#include <cstdlib>
#include <cstring>

namespace pbr {

namespace {

bool g_sandbox_mode = false;

bool EnvTruthy(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 &&
         std::strcmp(value, "FALSE") != 0;
}

} // namespace

void SetSandboxMode(const bool enabled) {
  g_sandbox_mode = enabled;
}

bool SandboxMode() {
  return g_sandbox_mode;
}

const char* BriefOrigin() {
  return g_sandbox_mode ? kSandboxBriefOrigin : kProductionBriefOrigin;
}

std::string BriefLlmBaseUrl() {
  return std::string(BriefOrigin()) + "/api/llm/v1";
}

std::string BriefRelayBaseUrl() {
  return std::string(BriefOrigin()) + "/api/relay";
}

std::string BriefMcpUrl() {
  return std::string(BriefOrigin()) + "/mcp";
}

const char* ProductDirBasename() {
  return g_sandbox_mode ? kSandboxProductDirName : kProductDirName;
}

std::string RewriteBriefOriginUrl(std::string url) {
  if (!g_sandbox_mode || url.empty()) {
    return url;
  }
  const std::string production_prefix = kProductionBriefOrigin;
  if (url.rfind(production_prefix, 0) == 0) {
    url.replace(0, production_prefix.size(), kSandboxBriefOrigin);
  }
  return url;
}

bool ResolveSandboxModeFromLaunch(const int argc, char** argv) {
  if (argv != nullptr) {
    for (int i = 1; i < argc; ++i) {
      if (std::strcmp(argv[i], "--sandbox") == 0) {
        return true;
      }
    }
  }
  return EnvTruthy("PP_BROWSER_SANDBOX");
}

} // namespace pbr
