#include "foundation/platform/CurlSsl.h"

#include "foundation/platform/os/OsTlsPlatformCurl.h"

namespace pbr {

void ApplyCurlSslDefaults(CURL* curl) {
  if (!curl) {
    return;
  }
  os::ApplyPlatformCurlSsl(curl);
}

}  // namespace pbr
