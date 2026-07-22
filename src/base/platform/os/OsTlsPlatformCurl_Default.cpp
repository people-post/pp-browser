#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)

#include "base/platform/os/OsTlsPlatformCurl.h"

namespace pbr::os {

void ApplyPlatformCurlSsl(CURL* /*curl*/) {
  // Desktop: curl uses Secure Transport (macOS), Schannel (Windows), or a
  // host CA bundle (Linux) — no extra setup required.
}

} // namespace pbr::os

#endif
