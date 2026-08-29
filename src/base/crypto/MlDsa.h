#pragma once

#include "base/crypto/CryptoTypes.h"

#include "common/Error.h"

#include "crypto/MlDsa.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

using ::pp::kMlDsa65PublicKeyBytes;
using ::pp::kMlDsa65SecretKeyBytes;
using ::pp::kMlDsa65SignatureBytes;
using ::pp::MlDsaKeyPair;
using ::pp::MlDsa;

/** Account ID: account:<base64url-unpadded(BLAKE2b-256(ML-DSA-65 pk))>. */
Roe<std::string> AccountIdFromMlDsaPublicKey(const ByteVector& public_key);

} // namespace pbr
