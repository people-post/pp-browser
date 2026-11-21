#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/TypedBlob.h"

#include "common/Error.h"
#include "common/Value.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {

/**
 * What broadcast's two signed JSON objects (the join ticket, the peer-announce tip; release-compatibility
 * SIGNED_FORMATS § 2.9) share in their readers. Their decoders parse before anything is verified, so they run step 1
 * of the receiver's order (docs/contracts/SIGNED_EXTENSIONS.md) themselves, before parsing; the verifiers then run the
 * whole order with `ReceiveSigned`.
 */

/**
 * Step 1 for a decoder: the outside `schema_version`. Missing or below `only`: a plain refusal (made by an older app);
 * above it: `AppError::NewerFormat` (update needed, never a forgery). Gives the version read.
 */
Roe<int> ReadSchemaVersion(const Object& json, std::string_view what, int only);

/** A string field: absent is `""`; present and not a string is malformed. */
Roe<std::string> ReadOptionalString(const Object& json, const std::string& key, std::string_view what);
/** An integer field: absent is `fallback`; present and not an integer is malformed. */
Roe<int64_t> ReadOptionalInt(const Object& json, const std::string& key, int64_t fallback, std::string_view what);
/** A base64 field's bytes: absent or `""` is empty; not a string or not base64 is malformed. */
Roe<ByteVector> ReadOptionalBase64(const std::string& b64, std::string_view what);

/** The publisher's raw ML-DSA-65 key (its PeerId's) as a read typed key, for `VerifyTyped`; malformed when not 1952 B. */
TypedRead PublisherSigningKey(const ByteVector& raw_public_key);

} // namespace pbr
