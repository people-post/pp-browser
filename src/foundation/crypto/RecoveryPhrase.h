#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** A recovery seed is 256 bits (multi-device M023 §2). */
inline constexpr size_t kRecoverySeedBytes = 32;
/** Its phrase: 24 BIP-0039 English words (256 bits + an 8-bit checksum). */
inline constexpr size_t kRecoveryPhraseWords = 24;

/**
 * The words for a 256-bit recovery seed, space-separated: BIP-0039 encoding (the seed as its entropy, the first
 * byte of SHA-256(seed) as the checksum). The seed itself is the root — not BIP-0039's PBKDF2 "seed", which this
 * app never uses.
 */
Roe<std::string> EncodeRecoveryPhrase(const ByteVector& seed);

/**
 * The seed a typed phrase names: 24 words from the list, separated by any whitespace, any letter case; an unknown
 * word, the wrong count or a failed checksum (a mistyped word) is an error that says which.
 */
Roe<ByteVector> DecodeRecoveryPhrase(const std::string& phrase);

} // namespace pbr
