#pragma once

#include "foundation/crypto/TypedBlob.h"

#include "common/Error.h"

#include <string>
#include <string_view>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Keys and signatures as this app holds them, turned into the typed blobs signed objects carry
 * (docs/contracts/SIGNED_EXTENSIONS.md § Typed blobs). The account and device signers (`IdentityStore::SignBytes`,
 * the device signer) answer with a bare ML-DSA-65 signature, and the identity, key cards and card devices may hold a
 * bare ML-DSA-65 key; objects on the wire hold only typed blobs. A held value that is already typed (its algorithm id
 * first) is read as it is. These read local values only: a field received from a peer or the relay is read with
 * `ReadTypedBlob*`, which takes typed blobs alone.
 */

/** A held signing public key (standard base64; bare ML-DSA-65 or typed) as a typed read. */
TypedRead ReadHeldSigningKeyB64(const std::string& held_b64, std::string_view what);

/** A held signing public key (bare ML-DSA-65 or typed) as the typed blob's standard base64. */
Roe<std::string> TypedSigningKeyB64FromHeld(const std::string& held_b64);

/** A signer's answer (a bare ML-DSA-65 signature, or a typed one; standard base64) as a typed signature's base64. */
Roe<std::string> TypedSignatureB64FromSigner(const std::string& signer_b64);

} // namespace pbr
