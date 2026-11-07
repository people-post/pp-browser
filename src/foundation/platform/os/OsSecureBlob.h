#pragma once

#include "common/Error.h"
#include "common/SecureBlobStore.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr::os {

/**
 * One small blob per `name`, kept where an app uninstall does not delete it (iOS: Keychain item,
 * this device only, never synced or backed up). Android 9+: a Keystore-sealed file in the no-backup
 * directory (an uninstall does delete it). Other platforms keep the user directory across
 * reinstalls and return `Unsupported` here. Behind lib/people SecureBlobStore.
 */
Roe<void> SecureBlobWrite(const std::string& name, const std::vector<uint8_t>& bytes);
/** nullopt when nothing is stored under `name`. */
Roe<std::optional<std::vector<uint8_t>>> SecureBlobRead(const std::string& name);
/** Succeeds when nothing is stored. */
Roe<void> SecureBlobDelete(const std::string& name);
/**
 * Write under an access level (iOS: kSecAttrAccessible, this device only). An item stored with another level is
 * replaced: deleted, then added with the new one (the old bytes are put back if the add fails).
 * kBiometryCurrentSet (iOS: kSecAttrAccessControl with BiometryCurrentSet) always deletes then adds, without
 * reading the old item (that read would prompt); writing needs no prompt. Unsupported elsewhere.
 */
Roe<void> SecureBlobWriteWithAccess(const std::string& name, const std::vector<uint8_t>& bytes,
                                    SecureBlobAccess access);
/**
 * Found / Missing / Unavailable / Canceled (iOS: OSStatus through MapKeychainStatus; the code rides along with
 * Unavailable and Canceled).
 */
Roe<pbr::SecureBlobLookup> SecureBlobLookup(const std::string& name);
/**
 * SecureBlobLookup for a kBiometryCurrentSet item: the system face / finger prompt shows `prompt` and this blocks
 * until it is answered, so never on the UI thread.
 */
Roe<pbr::SecureBlobLookup> SecureBlobLookupWithPrompt(const std::string& name, const std::string& prompt);
/** False on platforms with no uninstall-surviving store. */
bool SecureBlobSupported();

} // namespace pbr::os
