#pragma once

#include "base/crypto/CryptoTypes.h"

#include "common/Error.h"

#include <string>
#include <string_view>
#include "common/PbrCompat.h"

namespace pbr {

/** One-time libsodium init — safe to call repeatedly. */
void EnsureSodiumInit();

Roe<ByteVector> HexToBytes(std::string_view hex);
std::string BytesToHex(const ByteVector& bytes);

Roe<ByteVector> Base64Decode(const std::string& encoded);
std::string Base64Encode(const ByteVector& bytes);

} // namespace pbr
