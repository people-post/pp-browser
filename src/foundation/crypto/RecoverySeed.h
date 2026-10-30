#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include "common/Error.h"

#include "common/PbrCompat.h"

namespace pbr {

/**
 * What a recovery seed derives (multi-device M023 §2), each under its own HKDF-SHA256 label (salt
 * `pp-browser-recovery-v1`): the account keys, and the account locker's encryption key and signing key (the locker's
 * id is the hash of that signing key's public half, so a provider checks a write with no account knowledge). Device
 * keys are not derived — each device mints its own (M021 §1).
 */
struct RecoveryKeys {
  ByteVector account_ml_dsa_seed;   // 32 — MlDsa::GenerateKeyPairFromSeed
  ByteVector account_ml_kem_coins;  // 64 — HybridKem::GenerateKeyPairFromSeed
  ByteVector locker_key;            // 32 — AEAD key of locker snapshots
  ByteVector locker_signing_seed;   // 32 — MlDsa::GenerateKeyPairFromSeed; signs snapshots
};

/** A fresh 256-bit recovery seed (OS randomness). */
ByteVector GenerateRecoverySeed();

/** The keys a 32-byte recovery seed derives; the same seed always derives the same keys. */
Roe<RecoveryKeys> DeriveRecoveryKeys(const ByteVector& seed);

} // namespace pbr
