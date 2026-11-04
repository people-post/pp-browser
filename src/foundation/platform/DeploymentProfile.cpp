#include "foundation/platform/DeploymentProfile.h"

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
  const char* env = std::getenv("PP_BROWSER_ORIGIN");
  return env != nullptr && *env != '\0';
}

std::string BriefOrigin() {
  std::string origin;
  if (const char* env = std::getenv("PP_BROWSER_ORIGIN"); env && *env) {
    origin = env;
  } else if (ActiveBackend() == BackendEnv::Qa) {
    origin = CompiledQaOrigin();
  } else {
    origin = CompiledDefaultOrigin();
  }
  while (!origin.empty() && origin.back() == '/') {
    origin.pop_back();
  }
  return origin;
}

std::string BriefLlmBaseUrl() {
  return Derive("/api/llm/v1");
}

std::string BriefStreamUrlOverride() {
  const char* value = std::getenv("PP_BROWSER_BRIEF_STREAM_URL");
  return value ? std::string(value) : std::string();
}

std::string DiagnosticsUploadUrl() {
  if (const char* value = std::getenv("PP_BROWSER_DIAGNOSTICS_URL"); value && *value) {
    return value;
  }
  return Derive("/api/support/diagnostics");
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
