#pragma once

#include "common/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr::os {

/**
 * One small blob per `name`, kept where an app uninstall does not delete it (iOS: Keychain item,
 * this device only, never synced or backed up). Other platforms keep the user directory across
 * reinstalls and return `Unsupported` here. Local account backup (DESIGN-account-recovery §10).
 */
Roe<void> SecureBlobWrite(const std::string& name, const std::vector<uint8_t>& bytes);
/** nullopt when nothing is stored under `name`. */
Roe<std::optional<std::vector<uint8_t>>> SecureBlobRead(const std::string& name);
/** Succeeds when nothing is stored. */
Roe<void> SecureBlobDelete(const std::string& name);
/** False on platforms with no uninstall-surviving store. */
bool SecureBlobSupported();

} // namespace pbr::os
