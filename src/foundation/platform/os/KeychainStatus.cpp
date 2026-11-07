#include "foundation/platform/os/KeychainStatus.h"

namespace pbr::os {

namespace {

// Security.framework's values, spelled out so this file builds everywhere (SecBase.h).
constexpr int32_t kErrSecSuccess = 0;
constexpr int32_t kErrSecItemNotFound = -25300;
constexpr int32_t kErrSecUserCanceled = -128;
constexpr int32_t kErrSecAuthFailed = -25293;

} // namespace

KeychainLookup MapKeychainStatus(const int32_t os_status) {
  if (os_status == kErrSecSuccess) {
    return KeychainLookup::Found;
  }
  if (os_status == kErrSecItemNotFound) {
    return KeychainLookup::Missing;
  }
  if (os_status == kErrSecUserCanceled || os_status == kErrSecAuthFailed) {
    return KeychainLookup::Canceled;
  }
  return KeychainLookup::Unavailable; // incl. errSecInteractionNotAllowed (-25308)
}

} // namespace pbr::os
