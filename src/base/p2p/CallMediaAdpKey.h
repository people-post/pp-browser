#pragma once

#include "base/adp/Types.h"
#include "base/crypto/CryptoTypes.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <cstdint>
#include <string>

namespace pbr {

/** A009: derive ADP association key from call media key (never on wire). */
Roe<adp::PeerKey> DeriveCallMediaAdpAssocKey(const ByteVector& media_key, const std::string& call_id,
                                             uint32_t media_epoch);

/** Mint 16-byte assoc id (offerer); returns hex for hello `adp_assoc`. */
adp::AssocId MintCallMediaAdpAssocId();
std::string AssocIdToHex(const adp::AssocId& id);
Roe<adp::AssocId> AssocIdFromHex(const std::string& hex);

} // namespace pbr
