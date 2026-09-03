#if defined(__ANDROID__)

#include "foundation/platform/os/OsTlsCaPath.h"

#include <unistd.h>

namespace pbr::os {

const char* TlsCaPath() {
  // BoringSSL has no built-in trust store. Android ships hashed PEM CAs for
  // OpenSSL-style CAPATH. Prefer the Conscrypt apex path (API 34+) when present.
  static const char* path = []() -> const char* {
    static constexpr const char* kApex = "/apex/com.android.conscrypt/cacerts";
    static constexpr const char* kSystem = "/system/etc/security/cacerts";
    if (access(kApex, R_OK) == 0) {
      return kApex;
    }
    if (access(kSystem, R_OK) == 0) {
      return kSystem;
    }
    return nullptr;
  }();
  return path;
}

} // namespace pbr::os

#endif
