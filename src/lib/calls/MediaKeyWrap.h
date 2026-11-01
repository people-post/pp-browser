#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <string_view>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * A media key wrapped for one peer under a pairwise session key (V015): XChaCha20-Poly1305 with AAD
 * `<label>|<session_id>|<epoch>|<key_id>`. The label separates the protocols that wrap keys (calls,
 * broadcast), so a key wrapped for one never unwraps as the other's.
 */
std::string MediaKeyWrapAad(std::string_view label, const std::string& session_id, uint32_t epoch,
                            const std::string& key_id);
/** The wrapped key, base64 (nonce + ciphertext blob). */
Roe<std::string> WrapMediaKeyB64(std::string_view label, const ByteVector& session_key, const ByteVector& key_bytes,
                                 const std::string& session_id, uint32_t epoch, const std::string& key_id);
Roe<ByteVector> UnwrapMediaKeyB64(std::string_view label, const ByteVector& session_key,
                                  const std::string& wrapped_key_b64, const std::string& session_id, uint32_t epoch,
                                  const std::string& key_id);

} // namespace pbr
