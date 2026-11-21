#include "foundation/platform/DeploymentProfile.h"

#include "foundation/product/DevTools.h"

#include <cstdlib>
#include <optional>

#ifndef PP_RELAY_CONTENT_SHA256
#define PP_RELAY_CONTENT_SHA256 0
#endif

#ifndef PP_API_URL_PROD
#define PP_API_URL_PROD ""
#endif

#ifndef PP_RELAY_PEER_ID_PROD
#define PP_RELAY_PEER_ID_PROD ""
#endif

#ifndef PP_RELAY_PEER_ID_STAGING
#define PP_RELAY_PEER_ID_STAGING ""
#endif

#ifndef PP_RELAY_PEER_ID_DEV
#define PP_RELAY_PEER_ID_DEV ""
#endif

// Deprecated aliases of the staging variables.
#ifndef PP_RELAY_PEER_ID_QA
#define PP_RELAY_PEER_ID_QA ""
#endif

namespace pbr {

#ifndef PP_API_URL_STAGING
#define PP_API_URL_STAGING ""
#endif

#ifndef PP_API_URL_DEV
#define PP_API_URL_DEV ""
#endif

#ifndef PP_API_URL_QA
#define PP_API_URL_QA ""
#endif

namespace {

std::optional<BackendEnv>& Latched() {
  static std::optional<BackendEnv> latched;
  return latched;
}

std::string Derive(const std::string& origin, const char* path) {
  return origin.empty() ? std::string() : origin + path;
}

} // namespace

const char* CompiledDefaultOrigin() {
  return PP_API_URL_PROD;
}

const char* CompiledStagingOrigin() {
  return StagingOrLegacyQa(PP_API_URL_STAGING, PP_API_URL_QA);
}

const char* CompiledDevOrigin() {
  return PP_API_URL_DEV;
}

bool CompiledRelayContentSha256() {
  return PP_RELAY_CONTENT_SHA256 != 0;
}

const char* CompiledDefaultRelayPeerId() {
  return PP_RELAY_PEER_ID_PROD;
}

CompiledOrigins BuildCompiledOrigins() {
  return {.prod = CompiledDefaultOrigin(),
          .staging = CompiledStagingOrigin(),
          .dev = CompiledDevOrigin(),
          .prod_relay_peer_id = PP_RELAY_PEER_ID_PROD,
          .staging_relay_peer_id = StagingOrLegacyQa(PP_RELAY_PEER_ID_STAGING, PP_RELAY_PEER_ID_QA),
          .dev_relay_peer_id = PP_RELAY_PEER_ID_DEV};
}

const char* CompiledOriginFor(const BackendEnv env, const CompiledOrigins& compiled) {
  switch (env) {
    case BackendEnv::Staging:
      return compiled.staging;
    case BackendEnv::Dev:
      return compiled.dev;
    case BackendEnv::Prod:
      return compiled.prod;
    case BackendEnv::Custom:
      break;
  }
  return "";
}

bool BackendAvailableIn(const BackendEnv env, const CompiledOrigins& compiled) {
  if (env == BackendEnv::Staging || env == BackendEnv::Dev) {
    return *CompiledOriginFor(env, compiled) != '\0';
  }
  return true;
}

bool BackendAvailable(const BackendEnv env) {
  return BackendAvailableIn(env, BuildCompiledOrigins());
}

BackendEnv ParseBackendEnv(const std::string_view name) {
  if (name == "staging" || name == "qa") {
    return BackendEnv::Staging;
  }
  if (name == "dev") {
    return BackendEnv::Dev;
  }
  if (name == "custom") {
    return BackendEnv::Custom;
  }
  return BackendEnv::Prod;
}

const char* BackendEnvName(const BackendEnv env) {
  switch (env) {
    case BackendEnv::Staging:
      return "staging";
    case BackendEnv::Dev:
      return "dev";
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
  Latched() = ResolveLatchedBackend(requested, BuildCompiledOrigins());
}

BackendEnv ResolveLatchedBackend(const BackendEnv requested, const CompiledOrigins& compiled) {
  return BackendAvailableIn(requested, compiled) ? requested : BackendEnv::Prod;
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

std::string ResolveBriefOrigin(const bool ui_demo, const char* env_origin, const BackendEnv backend,
                               const CompiledOrigins& compiled) {
  if (ui_demo) {
    return {};
  }
  std::string origin;
  if (env_origin && *env_origin) {
    origin = env_origin;
  } else if (backend == BackendEnv::Staging || backend == BackendEnv::Dev) {
    origin = CompiledOriginFor(backend, compiled);
  } else {
    origin = compiled.prod;
  }
  while (!origin.empty() && origin.back() == '/') {
    origin.pop_back();
  }
  return origin;
}

std::string ResolveRelayPeerId(const bool ui_demo, const char* env_origin, const BackendEnv backend,
                               const CompiledOrigins& compiled, const char* env_peer_id) {
  if (ui_demo) {
    return {};
  }
  if (env_origin && *env_origin) {
    return env_peer_id ? std::string(env_peer_id) : std::string(); // the override's relay: its own PeerId, or none
  }
  if (backend == BackendEnv::Staging) {
    return compiled.staging_relay_peer_id;
  }
  return backend == BackendEnv::Dev ? compiled.dev_relay_peer_id : compiled.prod_relay_peer_id;
}

std::string BriefRelayPeerId() {
  return ResolveRelayPeerId(kUiDemoBuild, std::getenv("PP_BROWSER_ORIGIN"), ActiveBackend(), BuildCompiledOrigins(),
                            std::getenv("PP_BROWSER_RELAY_PEER_ID"));
}

std::string BriefOrigin() {
  return ResolveBriefOrigin(kUiDemoBuild, std::getenv("PP_BROWSER_ORIGIN"), ActiveBackend(), BuildCompiledOrigins());
}

std::string BriefLlmBaseUrlFor(const std::string& origin) {
  return Derive(origin, "/api/llm/v1");
}

std::string BriefRelayBaseUrlFor(const std::string& origin) {
  return Derive(origin, "/api/relay");
}

std::string BriefMcpUrlFor(const std::string& origin) {
  return Derive(origin, "/mcp");
}

std::string BriefLlmBaseUrl() {
  return BriefLlmBaseUrlFor(BriefOrigin());
}

std::string ResolveBriefStreamUrlOverride(const bool ui_demo, const char* env_url) {
  return (ui_demo || !env_url) ? std::string() : std::string(env_url);
}

std::string BriefStreamUrlOverride() {
  return ResolveBriefStreamUrlOverride(kUiDemoBuild, std::getenv("PP_BROWSER_BRIEF_STREAM_URL"));
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
  return BriefRelayBaseUrlFor(BriefOrigin());
}

std::string BriefMcpUrl() {
  return BriefMcpUrlFor(BriefOrigin());
}

const char* ProductDirBasename() {
  return kProductDirName;
}

} // namespace pbr
