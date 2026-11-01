#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include "common/Error.h"

#include <cstdint>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Restoring from a recovery phrase as a link-device import (multi-device M023 §8): the bundle the phrase
 * reconstructs — the account keys derived from `seed`, the seed itself, the relay binding and nickname the locker
 * held, and a **fresh** DEK (nothing on this install was sealed under the old one). No account card: the restored
 * install signs a fresh device list (M021 §8). Its JSON goes to `LinkDeviceCoordinator::Import`.
 */
Roe<std::string> BuildRecoveryBundleJson(const ByteVector& seed, const std::string& relay_user_id,
                                         const std::string& nickname, int64_t now_ms);

} // namespace pbr
