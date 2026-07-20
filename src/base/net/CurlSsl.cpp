#include "base/net/CurlSsl.h"

#include "base/platform/os/OsTlsCaPath.h"

namespace pbr {

void ApplyCurlSslDefaults(CURL* curl) {
  if (!curl) {
    return;
  }
  if (const char* ca_path = os::TlsCaPath()) {
    curl_easy_setopt(curl, CURLOPT_CAPATH, ca_path);
  }
}

} // namespace pbr
