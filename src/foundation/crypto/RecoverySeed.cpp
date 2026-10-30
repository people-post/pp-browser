#include "foundation/crypto/RecoverySeed.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/RecoveryPhrase.h"

#include <cstring>
#include <sodium.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr const char kRecoveryHkdfSalt[] = "pp-browser-recovery-v1";
constexpr const char kAccountMlDsaInfo[] = "account-ml-dsa";
constexpr const char kAccountMlKemInfo[] = "account-ml-kem";
constexpr const char kLockerIdInfo[] = "locker-id";
constexpr const char kLockerKeyInfo[] = "locker-key";
constexpr const char kLockerSigningInfo[] = "locker-signing";

Roe<ByteVector> Expand(const unsigned char* prk, const char* info, size_t length) {
  ByteVector out(length);
  if (crypto_kdf_hkdf_sha256_expand(out.data(), out.size(), info, std::strlen(info), prk) != 0) {
    return Error(std::string("recovery seed HKDF expand failed: ") + info);
  }
  return out;
}

} // namespace

ByteVector GenerateRecoverySeed() {
  EnsureSodiumInit();
  ByteVector seed(kRecoverySeedBytes);
  randombytes_buf(seed.data(), seed.size());
  return seed;
}

Roe<RecoveryKeys> DeriveRecoveryKeys(const ByteVector& seed) {
  EnsureSodiumInit();
  if (seed.size() != kRecoverySeedBytes) {
    return Error("recovery seed must be 32 bytes");
  }
  unsigned char prk[crypto_kdf_hkdf_sha256_KEYBYTES];
  if (crypto_kdf_hkdf_sha256_extract(prk, reinterpret_cast<const unsigned char*>(kRecoveryHkdfSalt),
                                     std::strlen(kRecoveryHkdfSalt), seed.data(), seed.size()) != 0) {
    return Error("recovery seed HKDF extract failed");
  }
  RecoveryKeys keys;
  struct Part {
    ByteVector* out;
    const char* info;
    size_t length;
  };
  const Part parts[] = {
      {&keys.account_ml_dsa_seed, kAccountMlDsaInfo, 32}, {&keys.account_ml_kem_coins, kAccountMlKemInfo, 64},
      {&keys.locker_id, kLockerIdInfo, 32},              {&keys.locker_key, kLockerKeyInfo, 32},
      {&keys.locker_signing_seed, kLockerSigningInfo, 32},
  };
  for (const Part& part : parts) {
    auto expanded = Expand(prk, part.info, part.length);
    if (!expanded) {
      sodium_memzero(prk, sizeof(prk));
      return expanded.error();
    }
    *part.out = std::move(*expanded);
  }
  sodium_memzero(prk, sizeof(prk));
  return keys;
}

} // namespace pbr
