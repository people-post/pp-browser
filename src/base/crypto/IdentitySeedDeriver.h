#pragma once

#include "base/crypto/CryptoTypes.h"

#include "common/Error.h"

#include "common/PbrCompat.h"

namespace pbr {

struct DerivedNodeIdentitySeeds {
  ByteVector device_ml_dsa_seed;   // 32
  ByteVector account_ml_dsa_seed;  // 32
  ByteVector account_ml_kem_coins; // 64
};

/**
 * Derive device + account PQ keygen seeds from one master identity seed via
 * HKDF-SHA256 (salt pp-node-identity-v1). Master must be ≥32 bytes.
 */
Roe<DerivedNodeIdentitySeeds> DeriveNodeIdentitySeeds(const ByteVector& master_seed);

} // namespace pbr
