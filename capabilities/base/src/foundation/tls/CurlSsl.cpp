#include "foundation/tls/CurlSsl.h"

#include "foundation/tls/OsTlsPlatformCurl.h"

namespace pbr {

void ApplyCurlSslDefaults(CURL* curl) {
  if (!curl) {
    return;
  }
  os::ApplyPlatformCurlSsl(curl);
}

} // namespace pbr
