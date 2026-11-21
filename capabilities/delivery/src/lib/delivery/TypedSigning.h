#pragma once

#include "foundation/crypto/TypedBlob.h"

#include "common/Error.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The seam between the delivery's signed objects (envelope v4, PeerRequestAuth v2: release-compatibility
 * SIGNED_FORMATS § 2.1, § 2.7) and the signers below them, which answer raw ML-DSA-65. The keys they are checked with
 * are held typed (cards and key stores, lib/people/CardKeys.h) and read with `ReadTypedBlobB64`.
 *
 * A signer the identity store gives (`IdentityStore::SignDeviceBytes`, `SignBytes`): the raw ML-DSA-65 signature of
 * the bytes, in standard base64.
 */
using RawSignFn = std::function<Roe<std::string>(const std::vector<uint8_t>&)>;

/** Sign `bytes` with `sign` and give the typed signature (docs/contracts/SIGNED_EXTENSIONS.md § Typed blobs), base64. */
Roe<std::string> SignTypedB64(const RawSignFn& sign, const std::vector<uint8_t>& bytes);

} // namespace pbr
