#include "lib/people/RecoveryBundle.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/LinkDeviceCodec.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/RecoverySeed.h"

#include <sodium.h>
#include "common/PbrCompat.h"

namespace pbr {

Roe<std::string> BuildRecoveryBundleJson(const ByteVector& seed, const std::string& relay_user_id,
                                         const std::string& nickname, const int64_t now_ms) {
  auto keys = DeriveRecoveryKeys(seed);
  if (!keys) {
    return keys.error();
  }
  auto signing = MlDsa::GenerateKeyPairFromSeed(keys->account_ml_dsa_seed);
  if (!signing) {
    return signing.error();
  }
  auto kem = HybridKem::GenerateKeyPairFromSeed(keys->account_ml_kem_coins);
  if (!kem) {
    return kem.error();
  }
  auto account_id = AccountIdFromMlDsaPublicKey(signing->public_key);
  if (!account_id) {
    return account_id.error();
  }
  EnsureSodiumInit();
  ByteVector dek(kDataEncryptionKeySize);
  randombytes_buf(dek.data(), dek.size());

  LinkDeviceBundleV1 bundle;
  bundle.account_id = *account_id;
  bundle.account_ml_dsa_pk_b64 = Base64Encode(signing->public_key);
  bundle.account_ml_dsa_sk_b64 = Base64Encode(signing->secret_key);
  bundle.account_kem_pk_b64 = Base64Encode(kem->public_key);
  bundle.account_kem_sk_b64 = Base64Encode(kem->private_key);
  bundle.dek_b64 = Base64Encode(dek);
  sodium_memzero(dek.data(), dek.size());
  bundle.relay_user_id = relay_user_id;
  bundle.nickname = nickname;
  bundle.created_at_ms = now_ms;
  bundle.expires_at_ms = now_ms + kLinkDeviceDefaultTtlMs;
  bundle.recovery_seed_b64 = Base64Encode(seed);
  return LinkDeviceCodec::Serialize(bundle);
}

} // namespace pbr
