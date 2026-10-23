#pragma once

#include "foundation/crypto/CryptoTypes.h"

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
/** RFC 4648 §5 base64url without padding (links, QR payloads). */
std::string Base64UrlEncode(const ByteVector& bytes);
Roe<ByteVector> Base64UrlDecode(const std::string& encoded);

} // namespace pbr
