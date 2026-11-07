#include "foundation/platform/os/OsSecureBlob.h"

namespace pbr::os {

// Desktop uninstalls leave the user directory in place, so there is nothing to survive. Android has its own file
// (Keystore + no-backup, OsSecureBlob_Android.cpp).
Roe<void> SecureBlobWrite(const std::string& /*name*/, const std::vector<uint8_t>& /*bytes*/) {
  return Error("Secure blob store is not supported on this platform");
}

Roe<std::optional<std::vector<uint8_t>>> SecureBlobRead(const std::string& /*name*/) {
  return std::optional<std::vector<uint8_t>>{};
}

Roe<void> SecureBlobDelete(const std::string& /*name*/) {
  return {};
}

Roe<void> SecureBlobWriteWithAccess(const std::string& name, const std::vector<uint8_t>& bytes,
                                    const SecureBlobAccess access) {
  if (access == SecureBlobAccess::kBiometryCurrentSet) {
    return Error("Biometry-bound secure blobs are not supported on this platform");
  }
  return SecureBlobWrite(name, bytes);
}

Roe<pbr::SecureBlobLookup> SecureBlobLookup(const std::string& /*name*/) {
  return pbr::SecureBlobLookup{};
}

Roe<pbr::SecureBlobLookup> SecureBlobLookupWithPrompt(const std::string& name, const std::string& /*prompt*/) {
  return SecureBlobLookup(name);
}

bool SecureBlobSupported() {
  return false;
}

} // namespace pbr::os
