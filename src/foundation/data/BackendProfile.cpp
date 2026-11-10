#include "foundation/data/BackendProfile.h"

#include "foundation/platform/DeploymentProfile.h"

#include <cctype>
#include <cstdint>

namespace pbr {

namespace {

std::string CustomKey(const std::string& url) {
  // FNV-1a 64: stable across platforms and releases, which std::hash is not.
  uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char c : NormalizeBackendUrl(url)) {
    hash = (hash ^ c) * 1099511628211ULL;
  }
  static const char* kHex = "0123456789abcdef";
  std::string key = "custom-";
  for (int shift = 44; shift >= 0; shift -= 4) {
    key.push_back(kHex[(hash >> shift) & 0xF]);
  }
  return key;
}

std::string KeyFor(const BackendEnv env, const std::string& registration_url) {
  switch (env) {
    case BackendEnv::Qa:
      return "qa";
    case BackendEnv::Custom:
      return CustomKey(registration_url);
    case BackendEnv::Prod:
      break;
  }
  return "prod";
}

} // namespace

std::string NormalizeBackendUrl(const std::string& url) {
  size_t begin = 0;
  size_t end = url.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(url[begin]))) {
    ++begin;
  }
  while (end > begin && (std::isspace(static_cast<unsigned char>(url[end - 1])) || url[end - 1] == '/')) {
    --end;
  }
  std::string out = url.substr(begin, end - begin);
  const size_t scheme_end = out.find("://");
  const size_t host_begin = scheme_end == std::string::npos ? 0 : scheme_end + 3;
  const size_t host_end = out.find('/', host_begin);
  for (size_t i = 0; i < (host_end == std::string::npos ? out.size() : host_end); ++i) {
    out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
  }
  return out;
}

std::string BackendProfileKey(const AppConfig& config) {
  if (OriginEnvOverrideActive()) {
    return CustomKey(BriefOrigin());
  }
  return KeyFor(ActiveBackend(), config.registration.base_url);
}

std::string SavedBackendKey(const AppConfig& config) {
  if (OriginEnvOverrideActive()) {
    return CustomKey(BriefOrigin());
  }
  BackendEnv env = ParseBackendEnv(config.backend.env);
  if (env == BackendEnv::Qa && !QaBackendAvailable()) {
    env = BackendEnv::Prod;
  }
  return KeyFor(
      env, config.backend.has_saved_urls ? config.backend.saved_registration.base_url : config.registration.base_url);
}

} // namespace pbr
