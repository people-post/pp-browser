#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * An account's binding of its KEM public key: the account signs, with its ML-DSA key, the domain
 * "pp-browser:kem-binding-v1", a version byte, its ML-DSA public key and the KEM public key. A peer then
 * takes a KEM key for an account only from a binding that account made (the relay that serves the
 * directory cannot substitute one). Empty when a key does not decode to its size.
 */
ByteVector KemBindingSignBytes(const std::string& signing_public_key_b64, const std::string& kem_public_key_b64);

/** `kem_public_key_b64` is `account_id`'s: its signing key hashes to that Account ID and signed the binding. */
Roe<void> VerifyKemBinding(const std::string& account_id, const std::string& signing_public_key_b64,
                           const std::string& kem_public_key_b64, const std::string& binding_signature_b64);

} // namespace pbr
