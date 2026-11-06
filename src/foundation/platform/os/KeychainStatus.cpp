#include "foundation/platform/os/KeychainStatus.h"

namespace pbr::os {

namespace {

// Security.framework's values, spelled out so this file builds everywhere (SecBase.h).
constexpr int32_t kErrSecSuccess = 0;
constexpr int32_t kErrSecItemNotFound = -25300;

} // namespace

KeychainLookup MapKeychainStatus(const int32_t os_status) {
  if (os_status == kErrSecSuccess) {
    return KeychainLookup::Found;
  }
  if (os_status == kErrSecItemNotFound) {
    return KeychainLookup::Missing;
  }
  return KeychainLookup::Unavailable; // incl. errSecInteractionNotAllowed (-25308)
}

} // namespace pbr::os
