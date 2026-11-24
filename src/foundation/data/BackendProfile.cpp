#include "foundation/data/BackendProfile.h"

#include "foundation/data/ProfileManifest.h"
#include "foundation/platform/DeploymentProfile.h"

#include <cctype>
#include <cstdint>
#include <string_view>

namespace pbr {

namespace {

constexpr std::string_view kStoreCustomPrefix = "custom-";
constexpr std::string_view kTesterCustomPrefix = "test-custom-";

} // namespace

namespace {

/** `<prefix><12 hex>` of the FNV-1a 64 of `text`: stable across platforms and releases, which std::hash is not. */
std::string HashedKey(const std::string_view prefix, const std::string& text) {
  uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char c : text) {
    hash = (hash ^ c) * 1099511628211ULL;
  }
  static const char* kHex = "0123456789abcdef";
  std::string key(prefix);
  for (int shift = 44; shift >= 0; shift -= 4) {
    key.push_back(kHex[(hash >> shift) & 0xF]);
  }
  return key;
}

std::string_view CustomPrefix(const BuildKind kind) {
  return kind == BuildKind::Tester ? kTesterCustomPrefix : kStoreCustomPrefix;
}

/** The URL's scheme and host (and port), no path: how a relay's origin is compared. */
std::string OriginOfUrl(const std::string& url) {
  const std::string normalized = NormalizeBackendUrl(url);
  const size_t scheme_end = normalized.find("://");
  const size_t host_begin = scheme_end == std::string::npos ? 0 : scheme_end + 3;
  const size_t path = normalized.find('/', host_begin);
  return path == std::string::npos ? normalized : normalized.substr(0, path);
}

std::string TrimmedId(const std::string& id) {
  size_t begin = 0;
  size_t end = id.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(id[begin]))) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(id[end - 1]))) {
    --end;
  }
  return id.substr(begin, end - begin);
}

} // namespace

std::string CustomBackendId(const std::string& registration_url, const std::string& org_relay_url,
                            const std::string& org_relay_id, const BuildKind kind) {
  // One field per line: a URL holds no newline, so two different triples never share a text.
  return HashedKey(CustomPrefix(kind), NormalizeBackendUrl(registration_url) + "\n" + OriginOfUrl(org_relay_url) +
                                           "\n" + TrimmedId(org_relay_id));
}

std::string BackendTableId(const BackendTable& table, const BuildKind kind) {
  if (table.id != "custom" && table.id != "test-custom") {
    return table.id;
  }
  return CustomBackendId(table.registration_url, table.org_relay_url, table.org_relay_id, kind);
}

std::string BackendIdFor(const BackendEnv env, const std::string& registration_url, const std::string& org_relay_url,
                         const std::string& org_relay_id, const CompiledOrigins& compiled) {
  switch (ResolveLatchedBackend(env, compiled)) {
    case BackendEnv::Staging:
      return "staging"; // a new environment (release-compatibility RC009 as amended): no profile made for qa is reused
    case BackendEnv::Dev:
      return "dev";
    case BackendEnv::Custom:
      return CustomBackendId(registration_url, org_relay_url, org_relay_id, compiled.kind);
    case BackendEnv::Prod:
      break;
  }
  return "prod"; // a store build's only: a tester build never resolves to Prod
}

std::string_view NetworkOfBackend(const std::string_view backend_id) {
  if (backend_id == "prod" || (backend_id.substr(0, kStoreCustomPrefix.size()) == kStoreCustomPrefix &&
                               backend_id.size() > kStoreCustomPrefix.size())) {
    return profile_network::kMain;
  }
  if (backend_id == "staging") {
    return profile_network::kStaging;
  }
  if (backend_id == "dev") {
    return profile_network::kDev;
  }
  if (backend_id.substr(0, kTesterCustomPrefix.size()) == kTesterCustomPrefix &&
      backend_id.size() > kTesterCustomPrefix.size()) {
    return profile_network::kTestCustom;
  }
  if (backend_id == profile_network::kNode) {
    return profile_network::kNode;
  }
  return {};
}

bool ProfileNetworkAllowed(const std::string_view network, const BuildKind kind) {
  if (network == profile_network::kStaging || network == profile_network::kDev) {
    return true;
  }
  return kind == BuildKind::Store ? network == profile_network::kMain : network == profile_network::kTestCustom;
}

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
  const CompiledOrigins compiled = BuildCompiledOrigins();
  if (OriginEnvOverrideActive()) {
    return BackendTableId(ActiveBackendTable(), compiled.kind);
  }
  return BackendIdFor(ActiveBackend(), config.registration.base_url, config.relay.base_url, config.relay.relay_id,
                      compiled);
}

std::string SavedBackendKey(const AppConfig& config) {
  if (OriginEnvOverrideActive()) {
    return BackendProfileKey(config);
  }
  // Under a built-in backend the loaded URLs are its own; the saved copies are the custom backend's.
  const bool saved = config.backend.has_saved_urls;
  const ServiceEndpointConfig& registration = saved ? config.backend.saved_registration : config.registration;
  const ServiceEndpointConfig& relay = saved ? config.backend.saved_relay : config.relay;
  return BackendIdFor(ParseBackendEnv(config.backend.env), registration.base_url, relay.base_url, relay.relay_id,
                      BuildCompiledOrigins());
}

} // namespace pbr
