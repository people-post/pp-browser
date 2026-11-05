#include "foundation/platform/DeploymentProfile.h"

#include "foundation/product/DevTools.h"

#include <cstdlib>
#include <optional>

#ifndef PP_API_URL_PROD
#define PP_API_URL_PROD ""
#endif

namespace pbr {

#ifndef PP_API_URL_QA
#define PP_API_URL_QA ""
#endif

namespace {

std::optional<BackendEnv>& Latched() {
  static std::optional<BackendEnv> latched;
  return latched;
}

std::string Derive(const char* path) {
  const std::string origin = BriefOrigin();
  return origin.empty() ? std::string() : origin + path;
}

} // namespace

const char* CompiledDefaultOrigin() {
  return PP_API_URL_PROD;
}

const char* CompiledQaOrigin() {
  return PP_API_URL_QA;
}

BackendEnv ParseBackendEnv(const std::string_view name) {
  if (name == "qa") {
    return BackendEnv::Qa;
  }
  if (name == "custom") {
    return BackendEnv::Custom;
  }
  return BackendEnv::Prod;
}

const char* BackendEnvName(const BackendEnv env) {
  switch (env) {
  case BackendEnv::Qa:
    return "qa";
  case BackendEnv::Custom:
    return "custom";
  case BackendEnv::Prod:
    break;
  }
  return "prod";
}

BackendEnv ActiveBackend() {
  return Latched().value_or(BackendEnv::Prod);
}

void LatchActiveBackend(BackendEnv requested) {
  if (Latched()) {
    return;
  }
  if (requested == BackendEnv::Qa && !QaBackendAvailable()) {
    requested = BackendEnv::Prod;
  }
  Latched() = requested;
}

bool ActiveBackendLatched() {
  return Latched().has_value();
}

void ResetActiveBackendForTest() {
  Latched().reset();
}

bool OriginEnvOverrideActive() {
  // The UI demo app ignores the variable (ResolveBriefOrigin), so its profile key never turns into custom-<hash>.
  const char* env = std::getenv("PP_BROWSER_ORIGIN");
  return !kUiDemoBuild && env != nullptr && *env != '\0';
}

std::string ResolveBriefOrigin(const bool ui_demo, const char* env_origin, const BackendEnv backend) {
  if (ui_demo) {
    return {};
  }
  std::string origin;
  if (env_origin && *env_origin) {
    origin = env_origin;
  } else if (backend == BackendEnv::Qa) {
    origin = CompiledQaOrigin();
  } else {
    origin = CompiledDefaultOrigin();
  }
  while (!origin.empty() && origin.back() == '/') {
    origin.pop_back();
  }
  return origin;
}

std::string BriefOrigin() {
  return ResolveBriefOrigin(kUiDemoBuild, std::getenv("PP_BROWSER_ORIGIN"), ActiveBackend());
}

std::string BriefLlmBaseUrl() {
  return Derive("/api/llm/v1");
}

std::string ResolveBriefStreamUrlOverride(const bool ui_demo, const char* env_url) {
  return (ui_demo || !env_url) ? std::string() : std::string(env_url);
}

std::string BriefStreamUrlOverride() {
  return ResolveBriefStreamUrlOverride(kUiDemoBuild, std::getenv("PP_BROWSER_BRIEF_STREAM_URL"));
}

std::string ResolveDiagnosticsUploadUrl(const bool ui_demo, const char* env_url, const std::string& origin) {
  if (ui_demo) {
    return {};
  }
  if (env_url && *env_url) {
    return env_url;
  }
  return origin.empty() ? std::string() : origin + "/api/support/diagnostics";
}

std::string DiagnosticsUploadUrl() {
  return ResolveDiagnosticsUploadUrl(kUiDemoBuild, std::getenv("PP_BROWSER_DIAGNOSTICS_URL"), BriefOrigin());
}

bool RunIfUploadAllowed(const bool ui_demo, const std::function<void()>& sender) {
  if (!UploadAllowed(ui_demo)) {
    return false;
  }
  if (sender) {
    sender();
  }
  return true;
}

std::string BriefRelayBaseUrl() {
  return Derive("/api/relay");
}

std::string BriefMcpUrl() {
  return Derive("/mcp");
}

const char* ProductDirBasename() {
  return kProductDirName;
}

} // namespace pbr
