#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/SignedReceive.h"

#include "common/Error.h"
#include "common/ExtArea.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * An account's binding of its KEM public key, v2 (docs/contracts/ACCOUNT_CARD.md § KEM binding; release-compatibility
 * SIGNED_FORMATS § 2.3): the account signs, with its ML-DSA key, its typed signing key and the typed KEM key it binds.
 * A peer takes a KEM key for an account only from a binding that account made, so whoever carries it (the relay
 * directory, a mesh directory row, a contact card) cannot substitute one.
 *
 *   signed: "pp-browser:kem-binding" 00 ‖ u8 2 ‖ LenBytes signing key (typed) ‖ LenBytes KEM key (typed) ‖ ext area
 *   blob:   u8 2 ‖ ext area ‖ LenBytes signature (typed)
 *
 * The binding rides other objects (key cards, directory rows, register/finish) as `kem_binding_b64`, the blob in
 * standard base64, so its version is the blob's first byte rather than a JSON field of each carrier. The blob keeps its
 * area as received: a binding with unknown non-critical entries is passed on as it came.
 */
inline constexpr char kKemBindingDomain[] = "pp-browser:kem-binding";
/** Release 1's version: the floor and the only one read. */
inline constexpr uint8_t kKemBindingVersion = 2;

/** The bytes the account signs, over the two typed keys (`TypedBlob.h`) and `ext`. */
ByteVector KemBindingSignBytes(std::span<const uint8_t> typed_signing_public_key,
                               std::span<const uint8_t> typed_kem_public_key, const pp::ExtArea& ext = {});

/** The blob carrying the account's typed signature over `KemBindingSignBytes` with the same `ext`. */
ByteVector KemBindingBlob(std::span<const uint8_t> typed_signature, const pp::ExtArea& ext = {});

/**
 * The account's raw ML-DSA-65 signature over the bytes, in standard base64 (`IdentityStore::SignBytes`,
 * `SignAccountBytes`).
 */
using KemBindingSignFn = std::function<Roe<std::string>(const ByteVector&)>;

/**
 * Bind `typed_kem_public_key_b64` to the account of `typed_signing_public_key_b64` (both typed, standard base64): the
 * `kem_binding_b64` blob. An error when a key does not read as this build's algorithm or `sign` fails.
 */
Roe<std::string> SignKemBindingB64(const std::string& typed_signing_public_key_b64,
                                   const std::string& typed_kem_public_key_b64, const KemBindingSignFn& sign,
                                   const pp::ExtArea& ext = {});

/**
 * Steps 1–5 of the receiver's order for a binding (SIGNED_EXTENSIONS § The receiver's order): the blob's version (1 or
 * none: Older; 3 and up: NeedsUpdate), its area and signature (Malformed), the three typed blobs (an unknown id:
 * NeedsUpdate), the signature over the rebuilt bytes (Forged), the area's critical entries (NeedsUpdate). Who signed it
 * is the caller's step 6 (`VerifyKemBinding`).
 */
SignedReceipt ReceiveKemBinding(const std::string& typed_signing_public_key_b64,
                                const std::string& typed_kem_public_key_b64, const std::string& binding_b64);

/**
 * `typed_kem_public_key_b64` is `account_id`'s: the binding is received (`ReceiveKemBinding`) and the signing key
 * hashes to that Account ID (the raw key, not the typed blob). An "update needed" receipt is `AppError::NewerFormat`;
 * every other failure a plain refusal.
 */
Roe<void> VerifyKemBinding(const std::string& account_id, const std::string& typed_signing_public_key_b64,
                           const std::string& typed_kem_public_key_b64, const std::string& binding_b64);

} // namespace pbr
