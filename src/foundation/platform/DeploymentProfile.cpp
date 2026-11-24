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

// CMake PP_TESTER_BUILD (release-compatibility RC022): a tester build never compiles a prod origin. CMake refuses one;
// this is the same rule where the origin is compiled in.
#ifndef PP_TESTER_BUILD
#define PP_TESTER_BUILD 0
#endif
static_assert(PP_TESTER_BUILD == 0 || sizeof(PP_API_URL_PROD) == 1,
              "a tester build (PP_TESTER_BUILD) has no prod origin");

// The org relays' ids (CMake PP_RELAY_ID_PROD / _STAGING / _DEV; kPendingRelayId by default) and their certificates
// at build time (config/relay-certs/<env>.cert, generated: kCompiledRelayCertificateProd / …Staging / …Dev).
#ifndef PP_RELAY_ID_PROD
#define PP_RELAY_ID_PROD ""
#endif

#ifndef PP_RELAY_ID_STAGING
#define PP_RELAY_ID_STAGING ""
#endif

#ifndef PP_RELAY_ID_DEV
#define PP_RELAY_ID_DEV ""
#endif

#include "CompiledRelayCertificates.inc"

namespace pbr {

#ifndef PP_API_URL_STAGING
#define PP_API_URL_STAGING ""
#endif

#ifndef PP_API_URL_DEV
#define PP_API_URL_DEV ""
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

BuildKind ThisBuildKind() {
  return PP_TESTER_BUILD != 0 ? BuildKind::Tester : BuildKind::Store;
}

bool IsTesterBuild() {
  return ThisBuildKind() == BuildKind::Tester;
}

const char* CompiledDefaultOrigin() {
  return PP_API_URL_PROD;
}

const char* CompiledStagingOrigin() {
  return PP_API_URL_STAGING;
}

const char* CompiledDevOrigin() {
  return PP_API_URL_DEV;
}

bool CompiledRelayContentSha256() {
  return PP_RELAY_CONTENT_SHA256 != 0;
}

const char* CompiledDefaultRelayId() {
  return PP_RELAY_ID_PROD;
}

const char* CompiledDefaultRelayCertificate() {
  return IsTesterBuild() ? "" : kCompiledRelayCertificateProd;
}

CompiledOrigins BuildCompiledOrigins() {
  // In a tester build CMake allows no prod origin and no prod relay id (only `pending` or none) and compiles no prod
  // certificate; the prod fields are never used there anyway (CompiledOriginFor, ResolveOrgRelayIdentity).
  return {.prod = CompiledDefaultOrigin(),
          .staging = CompiledStagingOrigin(),
          .dev = CompiledDevOrigin(),
          .prod_relay_id = PP_RELAY_ID_PROD,
          .staging_relay_id = PP_RELAY_ID_STAGING,
          .dev_relay_id = PP_RELAY_ID_DEV,
          .prod_relay_certificate = CompiledDefaultRelayCertificate(),
          .staging_relay_certificate = kCompiledRelayCertificateStaging,
          .dev_relay_certificate = kCompiledRelayCertificateDev,
          .kind = ThisBuildKind()};
}

const char* CompiledOriginFor(const BackendEnv env, const CompiledOrigins& compiled) {
  switch (env) {
    case BackendEnv::Staging:
      return compiled.staging;
    case BackendEnv::Dev:
      return compiled.dev;
    case BackendEnv::Prod:
      return compiled.kind == BuildKind::Tester ? "" : compiled.prod;
    case BackendEnv::Custom:
      break;
  }
  return "";
}

namespace {

/** The relay id compiled for `Staging` / `Dev` ("" for the others: this is only for the tester-build check). */
const char* TestEnvRelayIdFor(const BackendEnv env, const CompiledOrigins& compiled) {
  if (env == BackendEnv::Staging) {
    return compiled.staging_relay_id;
  }
  return env == BackendEnv::Dev ? compiled.dev_relay_id : "";
}

/**
 * The compiled environment whose origin and org relay `backend` takes: `Staging` / `Dev` their own (in a tester build
 * only when it can run them), anything else the fallback's — so a tester build never takes an environment it cannot use.
 */
BackendEnv CompiledEnvFor(const BackendEnv backend, const CompiledOrigins& compiled) {
  if ((backend == BackendEnv::Staging || backend == BackendEnv::Dev) &&
      (compiled.kind == BuildKind::Store || BackendAvailableIn(backend, compiled))) {
    return backend;
  }
  return FallbackBackendIn(compiled);
}

} // namespace

bool BackendAvailableIn(const BackendEnv env, const CompiledOrigins& compiled) {
  switch (env) {
    case BackendEnv::Staging:
    case BackendEnv::Dev:
      // A tester build has no prod to fall back on, so an environment whose relay is a placeholder (its operator set
      // not made yet: refused, so no account can be set up there) is not one it can use (release-compatibility RC022).
      return *CompiledOriginFor(env, compiled) != '\0' &&
             (compiled.kind == BuildKind::Store || !IsPendingRelayId(TestEnvRelayIdFor(env, compiled)));
    case BackendEnv::Prod:
      return compiled.kind == BuildKind::Store;
    case BackendEnv::Custom:
      break;
  }
  return true;
}

BackendEnv FallbackBackendIn(const CompiledOrigins& compiled) {
  if (compiled.kind == BuildKind::Store) {
    return BackendEnv::Prod;
  }
  for (const BackendEnv env : {BackendEnv::Staging, BackendEnv::Dev}) {
    if (BackendAvailableIn(env, compiled)) {
      return env;
    }
  }
  return BackendEnv::Custom;
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
  // Before any latch (no config file was loaded): what this build falls back to — never Prod in a tester build.
  return Latched() ? *Latched() : FallbackBackendIn(BuildCompiledOrigins());
}

void LatchActiveBackend(BackendEnv requested) {
  if (Latched()) {
    return;
  }
  Latched() = ResolveLatchedBackend(requested, BuildCompiledOrigins());
}

BackendEnv ResolveLatchedBackend(const BackendEnv requested, const CompiledOrigins& compiled) {
  return BackendAvailableIn(requested, compiled) ? requested : FallbackBackendIn(compiled);
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
  // Prod and Custom (whose own URLs are in the config file), and a staging / dev a tester build cannot use, take the
  // fallback's origin: prod's in a store build.
  std::string origin = (env_origin && *env_origin)
                           ? std::string(env_origin)
                           : std::string(CompiledOriginFor(CompiledEnvFor(backend, compiled), compiled));
  while (!origin.empty() && origin.back() == '/') {
    origin.pop_back();
  }
  return origin;
}

OrgRelayIdentity ResolveOrgRelayIdentity(const bool ui_demo, const char* env_origin, const BackendEnv backend,
                                         const CompiledOrigins& compiled, const char* env_relay_id) {
  if (ui_demo) {
    return {};
  }
  if (env_origin && *env_origin) {
    // The override's relay: its own relay id, or none; its certificate is fetched (the build's are other relays').
    return {.relay_id = env_relay_id ? std::string(env_relay_id) : std::string()};
  }
  // Prod and Custom take the fallback's: prod's in a store build; in a tester build the first usable one of staging's
  // and dev's, or none, never prod's (a custom backend there is no way to the prod relay's identity).
  switch (CompiledEnvFor(backend, compiled)) {
    case BackendEnv::Staging:
      return {.relay_id = compiled.staging_relay_id, .certificate_b64 = compiled.staging_relay_certificate};
    case BackendEnv::Dev:
      return {.relay_id = compiled.dev_relay_id, .certificate_b64 = compiled.dev_relay_certificate};
    case BackendEnv::Prod:
      if (compiled.kind == BuildKind::Store) {
        return {.relay_id = compiled.prod_relay_id, .certificate_b64 = compiled.prod_relay_certificate};
      }
      break;
    case BackendEnv::Custom:
      break;
  }
  return {};
}

OrgRelayIdentity BriefOrgRelayIdentity() {
  return ResolveOrgRelayIdentity(kUiDemoBuild, std::getenv("PP_BROWSER_ORIGIN"), ActiveBackend(),
                                 BuildCompiledOrigins(), std::getenv("PP_BROWSER_RELAY_ID"));
}

std::string BriefRelayId() {
  return BriefOrgRelayIdentity().relay_id;
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
