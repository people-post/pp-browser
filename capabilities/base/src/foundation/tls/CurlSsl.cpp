#include "foundation/tls/CurlSsl.h"

#include "foundation/tls/OsTlsPlatformCurl.h"

#if defined(PP_BROWSER_DEV_CA_BUNDLE)
#include <cstdlib>
#include <string>
#endif

namespace pbr {

namespace {

#if defined(PP_BROWSER_DEV_CA_BUNDLE)
/**
 * Lab and development only (docs/ops/CONFIGURATION.md `PP_BROWSER_CA_BUNDLE`): a PEM bundle that replaces curl's CA
 * bundle in Debug desktop builds, so the app lab's TLS relays (signed by a lab CA) are trusted. Read once; unset: curl's own.
 */
const std::string& CaBundleOverride() {
  static const std::string path = [] {
    const char* value = std::getenv("PP_BROWSER_CA_BUNDLE");
    return std::string(value != nullptr ? value : "");
  }();
  return path;
}
#endif

} // namespace

void ApplyCurlSslDefaults(CURL* curl) {
  if (!curl) {
    return;
  }
  os::ApplyPlatformCurlSsl(curl);
#if defined(PP_BROWSER_DEV_CA_BUNDLE)
  if (const std::string& bundle = CaBundleOverride(); !bundle.empty()) {
    curl_easy_setopt(curl, CURLOPT_CAINFO, bundle.c_str());
  }
#endif
}

} // namespace pbr
