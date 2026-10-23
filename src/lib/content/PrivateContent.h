#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "common/Error.h"

#include <string>
#include <string_view>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The profile's private content, addressed by its BLAKE2b-256 hash (the private CAS realm, under the
 * profile DEK). What a chat attachment, a pasted image or any other private object is stored as; who
 * may see it, and any RAM caching, is the caller's.
 */
bool PrivateContentExists(const std::string& profile_dir, const std::vector<uint8_t>& content_hash);

/** Store `plaintext` (its hash must be `content_hash`); returns the block path. */
Roe<std::string> SavePrivateContent(const std::string& profile_dir, std::string_view profile_id,
                                    const std::vector<uint8_t>& content_hash, const std::string& mime,
                                    const ByteVector& plaintext, const std::string& filename, const ByteVector& dek);

Roe<ByteVector> LoadPrivateContent(const std::string& profile_dir, std::string_view profile_id,
                                   const std::vector<uint8_t>& content_hash, const ByteVector& dek);

} // namespace pbr
