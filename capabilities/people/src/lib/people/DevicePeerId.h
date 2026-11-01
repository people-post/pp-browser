#pragma once

#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** A device's Amp PeerId, from its signing public key (base64): a device's mesh identity is its own key. */
Roe<std::string> DevicePeerIdFromKey(const std::string& device_public_key_b64);

} // namespace pbr
