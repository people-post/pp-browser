#pragma once

#include <cstdint>

namespace pbr::os {

/** What a Keychain read's OSStatus means for a caller that must not delete on doubt (PIN grace, #21 phase 1). */
enum class KeychainLookup { Found, Missing, Unavailable };

/**
 * errSecSuccess → Found, errSecItemNotFound → Missing, anything else (errSecInteractionNotAllowed while the device
 * is locked, unknown codes) → Unavailable: keep the item and try again later. Pure, so every platform tests it.
 */
KeychainLookup MapKeychainStatus(int32_t os_status);

} // namespace pbr::os
