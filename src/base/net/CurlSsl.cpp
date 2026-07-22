#include "base/net/CurlSsl.h"

#include "base/platform/os/OsTlsPlatformCurl.h"

namespace pbr {

void ApplyCurlSslDefaults(CURL* curl) {
  if (!curl) {
    return;
  }
  os::ApplyPlatformCurlSsl(curl);
}

} // namespace pbr
