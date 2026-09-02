#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "base/crypto/CryptoTypes.h"
#include "base/crypto/IDekConsumer.h"
#include "common/directory/IdentityTypes.h"

#include <mutex>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class IdentityStore : public Module, public IDekConsumer {
public:
  /** Current identity plaintext JSON schema inside identity.enc. Unversioned / v1–v2 Ed25519 device keys fail closed (PQ hard cut). */
  static constexpr int kSchemaVersion = 3;

  explicit IdentityStore(std::string data_dir, std::string profile_id = {});

  /** Optional master identity seed (≥32 bytes). When set:
   *  - empty identity.enc → mint device/account/KEM from HKDF (pp-node-identity-v1)
   *  - existing identity.enc → fail closed if derived keys ≠ stored keys
   * Call before LoadOrCreate (pp-node: PP_NODE_IDENTITY_SEED).
   */
  Roe<void> SetIdentitySeed(ByteVector master_seed);

  /** Required before LoadOrCreate/Get/Save — DEK from unlocked DataKeyVault. */
  Roe<void> SetDek(ByteVector dek) override;
  /**
   * Swap the in-memory DEK without discarding a loaded identity (link-device
   * import). Caller must Update/Save next so identity.enc is re-sealed.
   */
  Roe<void> ReplaceDekKeepLoaded(ByteVector dek);
  void ClearDek() override;

  Roe<LocalIdentity> LoadOrCreate();
  Roe<LocalIdentity> Get() const;
  Roe<LocalIdentity> Update(const LocalIdentity& identity);
  Roe<std::string> SignPayload(const std::string& canonical_json) const;
  Roe<std::string> SignBytes(const std::vector<uint8_t>& sign_bytes) const;
  /** Raw device ML-DSA-65 private key for libp2p Host identity binding. */
  Roe<ByteVector> GetDeviceMlDsaPrivateKey() const;
  Roe<ByteVector> GetDeviceMlDsaPublicKey() const;
  /** Account ML-KEM-768 secret (M015). Mints only if identity has no valid KEM yet. */
  Roe<ByteVector> GetOrCreateHybridKemPrivateKey() const;
  Roe<std::string> GetHybridKemPublicKeyB64() const;
  /** Account ML-DSA-65 secret (raw). Empty/error if not yet minted. */
  Roe<ByteVector> GetAccountMlDsaPrivateKey() const;
  Roe<std::string> GetAccountId() const;
  void Flush();

private:
  Roe<void> EnsureLoaded() const;
  Roe<void> Save() const;
  Roe<void> RequireDek() const;
  std::string StorePath() const;
  std::string ProfileId() const;

  std::string data_dir_;
  std::string profile_id_;
  mutable ByteVector dek_;
  mutable ByteVector identity_seed_;
  mutable std::mutex mutex_;
  mutable bool loaded_ = false;
  mutable LocalIdentity identity_;
  mutable std::vector<uint8_t> private_key_;
  mutable bool dirty_ = false;
};

} // namespace pbr
