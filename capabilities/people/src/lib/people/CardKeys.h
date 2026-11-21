#pragma once

#include "common/directory/KeyCard.h"
#include "foundation/crypto/CryptoTypes.h"

#include "common/Error.h"

#include "crypto/AlgId.h"

#include <functional>
#include <string>
#include <string_view>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The keys an account card and a key card carry are typed (docs/contracts/SIGNED_EXTENSIONS.md § Typed blobs; ACCOUNT_CARD
 * § Key card); an identity holds raw ones (`LocalIdentity`), and `MlDsa`, `HybridKem` and the identities (Account ID,
 * PeerId, fingerprints) take raw ones. These are the crossings.
 */

/**
 * A raw key as an identity holds it (standard base64) as the typed key a card carries, with this build's algorithm for
 * `kind`: an error when it does not decode to that algorithm's size.
 */
Roe<std::string> TypedKeyB64(pp::BlobKind kind, const std::string& raw_b64);

/**
 * The raw key a card's (or a key store's) typed field holds. An unknown algorithm id is `AppError::NewerFormat` (the
 * key's owner needs a newer app here); anything else that does not read is a plain error.
 */
Roe<ByteVector> RawSigningPublicKey(const std::string& typed_b64, std::string_view what = "signing key");
Roe<ByteVector> RawKemPublicKey(const std::string& typed_b64, std::string_view what = "KEM key");
/** The same in standard base64, for code that takes a raw key as base64. */
Roe<std::string> RawSigningPublicKeyB64(const std::string& typed_b64, std::string_view what = "signing key");
Roe<std::string> RawKemPublicKeyB64(const std::string& typed_b64, std::string_view what = "KEM key");

/**
 * A signer's answer (`IdentityStore::SignBytes`, `SignAccountBytes`, the device signer: a raw ML-DSA-65 signature,
 * base64) as the typed signature signed objects carry, base64.
 */
Roe<std::string> TypedSignatureB64(const std::string& raw_signature_b64);

/** The account's raw ML-DSA-65 signature over the bytes, base64 (`IdentityStore::SignBytes`, `SignAccountBytes`). */
using AccountRawSignFn = std::function<Roe<std::string>(const ByteVector&)>;

/**
 * The key card of an account whose raw keys an identity holds (`LocalIdentity::account_signing_public_key_b64`,
 * `kem_public_key_b64`): the keys typed, and the KEM binding v2 the account signs with `sign`
 * (foundation/crypto/KemKeyBinding.h). Our own card, pp-node's directory row, registration.
 */
Roe<KeyCard> MakeKeyCard(const std::string& account_id, const std::string& raw_signing_public_key_b64,
                         const std::string& raw_kem_public_key_b64, const AccountRawSignFn& sign);

} // namespace pbr
