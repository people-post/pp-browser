#include "foundation/platform/DeploymentProfile.h"

#include <cstdlib>

#ifndef PP_BROWSER_DEFAULT_ORIGIN
#define PP_BROWSER_DEFAULT_ORIGIN ""
#endif

namespace pbr {

namespace {

std::string Derive(const char* path) {
  const std::string origin = BriefOrigin();
  return origin.empty() ? std::string() : origin + path;
}

} // namespace

const char* CompiledDefaultOrigin() {
  return PP_BROWSER_DEFAULT_ORIGIN;
}

std::string BriefOrigin() {
  std::string origin;
  if (const char* env = std::getenv("PP_BROWSER_ORIGIN"); env && *env) {
    origin = env;
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
