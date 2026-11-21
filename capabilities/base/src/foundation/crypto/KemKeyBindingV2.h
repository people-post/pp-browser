#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include "common/ExtArea.h"

#include <span>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * KEM binding v2, the producer only (release-compatibility SIGNED_FORMATS § 2.3), for the directory row pp-node writes
 * (mesh v10 reads a row's binding as this blob, docs/DIRECTORY.md in pp-cpp-mesh). A stopgap of the cutover's pin
 * change: the KEM binding's v2 (producing and verifying it, the key card's `kem_binding_b64`) is cutover item W2's, in
 * `KemKeyBinding`, which replaces this file. Until then `KemKeyBinding` is still v1, and nothing in the app reads v2.
 *
 *   signed: "pp-browser:kem-binding" 00 ‖ u8 2 ‖ LenBytes signing key (typed) ‖ LenBytes KEM key (typed) ‖ ext area
 *   blob:   u8 2 ‖ ext area ‖ LenBytes signature (typed)
 */
inline constexpr uint8_t kKemBindingV2Version = 2;

/** The bytes the account signs: its typed signing key and the typed KEM key it binds (`TypedBlob.h`). */
ByteVector KemBindingV2SignBytes(std::span<const uint8_t> typed_signing_public_key,
                                 std::span<const uint8_t> typed_kem_public_key, const pp::ExtArea& ext = {});

/** The blob that carries the account's typed signature over `KemBindingV2SignBytes` (with the same `ext`). */
ByteVector KemBindingV2Blob(std::span<const uint8_t> typed_signature, const pp::ExtArea& ext = {});

} // namespace pbr
