#include "foundation/tls/CurlSsl.h"

#include "foundation/tls/OsTlsPlatformCurl.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <cstdlib>
#include <string>

namespace pbr {

namespace {

#if !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
/**
 * Lab and development only (docs/ops/CONFIGURATION.md `PP_BROWSER_CA_BUNDLE`): a PEM bundle that replaces curl's CA
 * bundle on desktop builds, so the app lab's TLS relays (signed by a lab CA) are trusted. Read once; unset: curl's own.
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
#if !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
  if (const std::string& bundle = CaBundleOverride(); !bundle.empty()) {
    curl_easy_setopt(curl, CURLOPT_CAINFO, bundle.c_str());
  }
#endif
}

} // namespace pbr
