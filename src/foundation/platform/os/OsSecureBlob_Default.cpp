#include "foundation/platform/os/OsSecureBlob.h"

namespace pbr::os {

// Desktop uninstalls leave the user directory in place, so there is nothing to survive; Android
// (Keystore + no-backup) is still to come.
Roe<void> SecureBlobWrite(const std::string& /*name*/, const std::vector<uint8_t>& /*bytes*/) {
  return Error("Secure blob store is not supported on this platform");
}

Roe<std::optional<std::vector<uint8_t>>> SecureBlobRead(const std::string& /*name*/) {
  return std::optional<std::vector<uint8_t>>{};
}

Roe<void> SecureBlobDelete(const std::string& /*name*/) {
  return {};
}

bool SecureBlobSupported() {
  return false;
}

} // namespace pbr::os
