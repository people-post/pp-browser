#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

namespace pbr {

inline constexpr size_t kAttachmentContentHashSize = 32;

/** BLAKE2b-256 of attachment plaintext (D075 / R016). */
Roe<ByteVector> AttachmentContentHash(const ByteVector& plaintext);

} // namespace pbr
