#pragma once

#include <cstdint>

namespace pbr::os {

/** What a Keychain read's OSStatus means for a caller that must not delete on doubt (PIN grace, #21 phase 1). */
enum class KeychainLookup { Found, Missing, Unavailable, Canceled };

/**
 * errSecSuccess → Found, errSecItemNotFound → Missing (for a biometry-bound item also: the enrolment changed),
 * errSecUserCanceled (-128) and errSecAuthFailed (-25293, the face / finger did not pass or biometry is locked out)
 * → Canceled, anything else (errSecInteractionNotAllowed while the device is locked or the prompt could not show,
 * unknown codes) → Unavailable: keep the item and try again later. Pure, so every platform tests it.
 */
KeychainLookup MapKeychainStatus(int32_t os_status);

} // namespace pbr::os
