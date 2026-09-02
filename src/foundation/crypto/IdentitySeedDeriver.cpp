#include "foundation/crypto/IdentitySeedDeriver.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"

#include <cstring>
#include <sodium.h>
#include "common/PbrCompat.h"

namespace pbr {

Roe<DerivedNodeIdentitySeeds> DeriveNodeIdentitySeeds(const ByteVector& master_seed) {
  EnsureSodiumInit();
  if (master_seed.size() < 32) {
    return Error("identity seed must be at least 32 bytes");
  }

  unsigned char prk[crypto_kdf_hkdf_sha256_KEYBYTES];
  if (crypto_kdf_hkdf_sha256_extract(
          prk, reinterpret_cast<const unsigned char*>(kNodeIdentityHkdfSalt),
          std::strlen(kNodeIdentityHkdfSalt), master_seed.data(), master_seed.size()) != 0) {
    return Error("identity seed HKDF extract failed");
  }

  DerivedNodeIdentitySeeds out;
  out.device_ml_dsa_seed.resize(32);
  out.account_ml_dsa_seed.resize(32);
  out.account_ml_kem_coins.resize(64);

  if (crypto_kdf_hkdf_sha256_expand(out.device_ml_dsa_seed.data(), out.device_ml_dsa_seed.size(),
                                    kNodeIdentityMlDsaDeviceInfo, std::strlen(kNodeIdentityMlDsaDeviceInfo),
                                    prk) != 0) {
    sodium_memzero(prk, sizeof(prk));
    return Error("identity seed HKDF expand (device ML-DSA) failed");
  }
  if (crypto_kdf_hkdf_sha256_expand(out.account_ml_dsa_seed.data(), out.account_ml_dsa_seed.size(),
                                    kNodeIdentityMlDsaAccountInfo, std::strlen(kNodeIdentityMlDsaAccountInfo),
                                    prk) != 0) {
    sodium_memzero(prk, sizeof(prk));
    return Error("identity seed HKDF expand (account ML-DSA) failed");
  }
  if (crypto_kdf_hkdf_sha256_expand(out.account_ml_kem_coins.data(), out.account_ml_kem_coins.size(),
                                    kNodeIdentityMlKemAccountInfo, std::strlen(kNodeIdentityMlKemAccountInfo),
                                    prk) != 0) {
    sodium_memzero(prk, sizeof(prk));
    return Error("identity seed HKDF expand (account ML-KEM) failed");
  }
  sodium_memzero(prk, sizeof(prk));
  return out;
}

} // namespace pbr
