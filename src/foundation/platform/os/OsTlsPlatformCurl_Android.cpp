#if defined(__ANDROID__)

#include "foundation/platform/os/OsTlsPlatformCurl.h"

#include "foundation/platform/os/OsTlsCaPath.h"

#include <curl/curl.h>

namespace pbr::os {

void ApplyPlatformCurlSsl(CURL* curl) {
  if (!curl) {
    return;
  }
  // BoringSSL has no built-in trust store. Android ships hashed PEM CAs for
  // OpenSSL-style CAPATH — that directory *is* the platform trust store and
  // tracks OS CA updates (see OsTlsCaPath_Android.cpp).
  if (const char* ca_path = TlsCaPath()) {
    curl_easy_setopt(curl, CURLOPT_CAPATH, ca_path);
  }
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
}

} // namespace pbr::os

#endif // __ANDROID__
