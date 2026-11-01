#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "common/Error.h"

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * This device's account id and KEM keys (people's): what decrypting, PSK rotation and group membership
 * need of the local identity — the account's KEM opens account copies, the device's its device pairs (M021).
 */
struct LocalKeys {
  std::function<Roe<std::string>()> account_id;
  std::function<Roe<ByteVector>()> kem_private_key;
  /** This device's PeerId: copies for it are its own; other devices' are its siblings'. */
  std::function<Roe<std::string>()> device_peer_id;
  std::function<Roe<ByteVector>()> device_kem_private_key;
};

} // namespace pbr
